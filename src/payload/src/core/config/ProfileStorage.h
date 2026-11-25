#pragma once
#include <filesystem>
#include <system_error>
#include <nlohmann/json.hpp>

namespace dt::storage {
// Owner token assembled at runtime — never stored as a constexpr literal so
// it does not survive into .rdata in plaintext. XOR table, key 0x1F.
// Decodes to: "D3DProfileConfig:v1"
inline std::string profileOwnerToken() {
    static const unsigned char k[] = {
        0x44^0x1F, 0x33^0x1F, 0x44^0x1F, 0x50^0x1F, 0x72^0x1F,
        0x6F^0x1F, 0x66^0x1F, 0x69^0x1F, 0x6C^0x1F, 0x65^0x1F,
        0x43^0x1F, 0x6F^0x1F, 0x6E^0x1F, 0x66^0x1F, 0x69^0x1F,
        0x67^0x1F, 0x3A^0x1F, 0x76^0x1F, 0x31^0x1F
    };
    std::string result;
    result.reserve(sizeof(k));
    for (auto b : k) result.push_back(static_cast<char>(b ^ 0x1F));
    return result;
}
std::filesystem::path profileRoot(const std::filesystem::path& localAppData);
bool validProfileRoot(const std::filesystem::path& candidate, const std::filesystem::path& localAppData);
// Copies recognized old profiles without overwriting or deleting originals.
std::filesystem::path prepareProfileRoot(const std::filesystem::path& localAppData);
std::string encodeProfileData(const std::string& jsonText);
std::string decodeProfileData(std::string_view fileBytes);
// Read, decode and parse a profile file. Returns a discarded value on any error.
nlohmann::json readProfile(const std::filesystem::path& path);
struct PurgeResult {
    bool valid = false;
    unsigned removed = 0;
    unsigned preserved = 0;
    std::error_code error;
};
// Removes marked profile files only. Never recursively deletes a directory.
PurgeResult purgeProfiles(const std::filesystem::path& candidate,
                          const std::filesystem::path& localAppData);
}
