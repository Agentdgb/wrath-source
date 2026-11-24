#pragma once
#ifndef WRATH_OBFUSCATED_STRING_H
#define WRATH_OBFUSCATED_STRING_H

#include <string>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wrath::crypto
{

// FNV-1a 32-bit en tiempo de compilación para derivar una clave por cadena.
// Mucho menos predecible que __TIME__ solo y resiste análisis estático simple.
constexpr uint32_t Fnv1a32(const char* str, size_t len, uint32_t seed = 2166136261u)
{
    uint32_t hash = seed;
    for (size_t i = 0; i < len; ++i)
    {
        hash ^= static_cast<uint8_t>(str[i]);
        hash *= 16777619u;
    }
    return hash;
}

// Semilla compuesta: contenido de la cadena + linea + __TIME__ para evitar
// colisiones entre strings idénticas en distintas lineas.
constexpr uint32_t CompileTimeSeed(const char* str, size_t len)
{
    constexpr const char time[] = __TIME__;
    uint32_t tseed = Fnv1a32(time, sizeof(time) - 1);
    return Fnv1a32(str, len, tseed + static_cast<uint32_t>(__LINE__));
}

// S-box ligera de 256 entradas generada en tiempo de compilación para un
// segundo paso de substitucion que rompe patrones de XOR simple.
constexpr std::array<uint8_t, 256> BuildSBox()
{
    std::array<uint8_t, 256> s{};
    for (int i = 0; i < 256; ++i)
        s[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i)
    {
        j = static_cast<uint8_t>(j + s[i] + static_cast<uint8_t>((i * 31) ^ 0x5A));
        std::swap(s[i], s[j]);
    }
    return s;
}

template <size_t N, uint32_t Key>
class CryptString
{
public:
    constexpr CryptString(const char(&str)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            // Paso 1: XOR con clave derivada (FNV-1a) rotada por posicion
            uint8_t k = static_cast<uint8_t>((Key >> ((i * 5) % 32)) & 0xFF);
            m_encrypted[i] = static_cast<char>(str[i] ^ k);
        }
    }

    // Descifra al vuelo en la pila durante la ejecución con S-box + XOR rotado
    std::string decrypt() const
    {
        static constexpr auto sbox = BuildSBox();
        std::string result;
        result.resize(N - 1); // Excluir null-terminator
        for (size_t i = 0; i < N - 1; ++i)
        {
            uint8_t k = static_cast<uint8_t>((Key >> ((i * 5) % 32)) & 0xFF);
            uint8_t b = static_cast<uint8_t>(m_encrypted[i]) ^ k;
            // Rotate left 3 bits y pasar por S-box
            b = static_cast<uint8_t>((b << 3) | (b >> 5));
            result[i] = static_cast<char>(sbox[b] ^ k);
        }
        return result;
    }

private:
    std::array<char, N> m_encrypted{};
};

// Macro conveniente para ocultar cadenas de binarios y decompiladores (IDA/Ghidra/strings)
#define CRYPT_STR(str) ([]() { \
    constexpr auto crypted = wrath::crypto::CryptString<sizeof(str), wrath::crypto::CompileTimeSeed(str, sizeof(str))>(str); \
    return crypted.decrypt(); \
})()

} // namespace wrath::crypto

#endif // WRATH_OBFUSCATED_STRING_H
