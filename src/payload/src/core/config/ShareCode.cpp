#include "core/config/ShareCode.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace dt::config {
namespace {
constexpr std::size_t maxPayload = 256 * 1024;
constexpr std::string_view shareCodePrefix = "SC1.";
constexpr std::size_t identifierBytes = 8;
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
std::uint32_t checksum(std::string_view text) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char c : text) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
std::string encode(std::string_view text) {
    std::string result;
    result.reserve((text.size() * 4 + 2) / 3);
    std::uint32_t value = 0;
    int bits = 0;
    for (unsigned char c : text) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; result.push_back(alphabet[(value >> bits) & 63]); }
    }
    if (bits) result.push_back(alphabet[(value << (6 - bits)) & 63]);
    return result;
}
std::string decode(std::string_view text) {
    if (text.size() % 4 == 1 || text.size() > (maxPayload * 4 + 2) / 3)
        throw std::runtime_error("Invalid or oversized code");
    std::string result;
    result.reserve(text.size() * 3 / 4);
    std::uint32_t value = 0;
    int bits = 0;
    for (char c : text) {
        const auto index = std::string_view(alphabet, 64).find(c);
        if (index == std::string_view::npos) throw std::runtime_error("Invalid code characters");
        value = (value << 6) | static_cast<std::uint32_t>(index);
        bits += 6;
        if (bits >= 8) { bits -= 8; result.push_back(static_cast<char>((value >> bits) & 255)); }
    }
    if ((bits && (value & ((1u << bits) - 1))) || result.size() > maxPayload)
        throw std::runtime_error("Invalid code encoding");
    return result;
}
}

std::string encodeShareCode(const SharedProfile& profile) {
    if (!profile.settings.is_object() || profile.name.empty() || profile.name.size() > 128)
        throw std::runtime_error("Invalid profile");
    const auto payload = nlohmann::json{{"name", profile.name}, {"settings", profile.settings}}.dump();
    if (payload.size() > maxPayload) throw std::runtime_error("Config is too large to share");
    std::array<unsigned char, identifierBytes> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Could not generate code identifier");
    std::string id;
    constexpr char hex[] = "0123456789ABCDEF";
    for (auto c : random) { id += hex[c >> 4]; id += hex[c & 15]; }
    std::string code = std::string(shareCodePrefix) + id + "." + encode(payload);
    char crc[9]{};
    std::snprintf(crc, sizeof(crc), "%08X", checksum(code));
    return code + "." + crc;
}

SharedProfile decodeShareCode(std::string_view raw) {
    if (raw.size() > maxPayload * 2) throw std::runtime_error("Code is too large");
    std::string code;
    code.reserve(raw.size());
    for (unsigned char c : raw) if (!std::isspace(c)) code.push_back(static_cast<char>(c));
    if (!code.starts_with(shareCodePrefix)) throw std::runtime_error("Unsupported code format");
    const auto idEnd = code.find('.', shareCodePrefix.size());
    const auto bodyEnd = code.find('.', idEnd == std::string::npos ? code.size() : idEnd + 1);
    if (idEnd != shareCodePrefix.size() + identifierBytes * 2
        || bodyEnd == std::string::npos || code.size() - bodyEnd != 9)
        throw std::runtime_error("Incomplete code");
    for (std::size_t i = shareCodePrefix.size(); i < idEnd; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(code[i]))) throw std::runtime_error("Invalid identifier");
    std::uint32_t expected = 0;
    const auto parsed = std::from_chars(code.data() + bodyEnd + 1, code.data() + code.size(), expected, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size()
        || checksum(std::string_view(code).substr(0, bodyEnd)) != expected)
        throw std::runtime_error("Code is damaged or incomplete");
    const auto payload = decode(std::string_view(code).substr(idEnd + 1, bodyEnd - idEnd - 1));
    std::size_t nodes = 0;
    auto envelope = nlohmann::json::parse(payload, [&nodes](int depth, auto, auto&) {
        if (depth > 32 || ++nodes > 50000) throw std::runtime_error("Config is too complex");
        return true;
    });
    if (!envelope.is_object() || !envelope.contains("name") || !envelope["name"].is_string()
        || !envelope.contains("settings") || !envelope["settings"].is_object())
        throw std::runtime_error("Invalid config contents");
    auto name = envelope["name"].get<std::string>();
    if (name.empty() || name.size() > 128) throw std::runtime_error("Invalid config name");
    return {std::move(name), std::move(envelope["settings"])};
}
}
