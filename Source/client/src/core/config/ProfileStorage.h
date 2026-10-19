#pragma once
#include <filesystem>
#include <system_error>
#include <nlohmann/json.hpp>

namespace vanta::storage {
inline constexpr char profileOwner[] = "D3DProfileConfig:v1";
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
