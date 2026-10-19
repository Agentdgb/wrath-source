#include "core/config/ProfileStorage.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <set>

namespace vanta::storage {

std::string encodeProfileData(const std::string& jsonText) {
    if (jsonText.empty()) return {};
    std::string out;
    out.reserve(8 + jsonText.size());
    const char magic[4] = {'D', '3', 'D', 'S'};
    out.append(magic, 4);
    const char ver[4] = {0x01, 0x00, 0x02, 0x00};
    out.append(ver, 4);
    static const std::uint8_t keyTable[16] = {
        0x5B, 0x9A, 0x2C, 0xF1, 0x7E, 0x43, 0x88, 0x6D,
        0xA5, 0x3F, 0x12, 0xC4, 0x8B, 0xE7, 0x90, 0x24
    };
    for (std::size_t i = 0; i < jsonText.size(); ++i) {
        std::uint8_t b = static_cast<std::uint8_t>(jsonText[i]);
        std::uint8_t k = keyTable[i % 16] ^ static_cast<std::uint8_t>((i * 7) & 0xFF);
        out.push_back(static_cast<char>(b ^ k));
    }
    return out;
}

std::string decodeProfileData(std::string_view fileBytes) {
    if (fileBytes.size() >= 8 && fileBytes[0] == 'D' && fileBytes[1] == '3' && fileBytes[2] == 'D' && fileBytes[3] == 'S') {
        std::string out;
        std::size_t payloadSize = fileBytes.size() - 8;
        out.reserve(payloadSize);
        static const std::uint8_t keyTable[16] = {
            0x5B, 0x9A, 0x2C, 0xF1, 0x7E, 0x43, 0x88, 0x6D,
            0xA5, 0x3F, 0x12, 0xC4, 0x8B, 0xE7, 0x90, 0x24
        };
        for (std::size_t i = 0; i < payloadSize; ++i) {
            std::uint8_t b = static_cast<std::uint8_t>(fileBytes[8 + i]);
            std::uint8_t k = keyTable[i % 16] ^ static_cast<std::uint8_t>((i * 7) & 0xFF);
            out.push_back(static_cast<char>(b ^ k));
        }
        return out;
    }
    return std::string(fileBytes);
}

namespace {
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool plainDirectory(FileHandle& handle, const std::filesystem::path& path) {
    handle.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle.value, &info)
        && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool safeAbsolute(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || path == path.root_path()) return false;
    for (const auto& part : path)
        if (part == L"." || part == L"..") return false;
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        } else {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
        if (current == current.root_path()) break;
    }
    return true;
}

bool writeNew(const std::filesystem::path& path, const std::string& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
        && written == data.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!ok) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    if (!CloseHandle(file)) ok = false;
    return ok;
}

// Read ownership and request deletion on the same locked file handle. A path
// replacement between validation and deletion cannot redirect this operation.
bool deleteOwnedProfile(const std::filesystem::path& path, std::error_code& error) {
    FileHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        error = std::error_code(GetLastError(), std::system_category());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!GetFileInformationByHandle(file.value, &info) || !GetFileSizeEx(file.value, &length)) {
        error = std::error_code(GetLastError(), std::system_category());
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        || length.QuadPart <= 0 || length.QuadPart > 2 * 1024 * 1024) return false;
    std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        || read != bytes.size()) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    std::string decoded = decodeProfileData(bytes);
    const auto data = nlohmann::json::parse(decoded, nullptr, false);
    if (!data.is_object()) return false;
    const auto owner = data.find("#owner");
    if (owner == data.end() || !owner->is_string() || *owner != profileOwner) return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        error = std::error_code(GetLastError(), std::system_category());
        return false;
    }
    return true;
}

void deleteActiveMarker(const std::filesystem::path& path, const std::set<std::string>& removed,
                       std::error_code& error) {
    FileHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND) error = std::error_code(code, std::system_category());
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!GetFileInformationByHandle(file.value, &info) || !GetFileSizeEx(file.value, &length)) {
        error = std::error_code(GetLastError(), std::system_category()); return;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || length.QuadPart <= 0 || length.QuadPart > 512) return;
    std::string name(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD count = 0;
    if (!ReadFile(file.value, name.data(), static_cast<DWORD>(name.size()), &count, nullptr)
        || count != name.size()) { error = std::make_error_code(std::errc::io_error); return; }
    while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
    if (!removed.contains(name)) return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)))
        error = std::error_code(GetLastError(), std::system_category());
}
}

nlohmann::json readProfile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!safeAbsolute(path) || !std::filesystem::is_regular_file(path, ec)
        || ec || std::filesystem::file_size(path, ec) > 2 * 1024 * 1024 || ec) return {};
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string decoded = decodeProfileData(bytes);
    return nlohmann::json::parse(decoded, nullptr, false);
}

std::filesystem::path profileRoot(const std::filesystem::path& localAppData) {
    if (!safeAbsolute(localAppData)) return {};
    return localAppData / L"D3DSCache" / L"LocalCache";
}

bool validProfileRoot(const std::filesystem::path& candidate, const std::filesystem::path& localAppData) {
    const auto expected = profileRoot(localAppData);
    if (expected.empty() || !safeAbsolute(candidate)) return false;
    return _wcsicmp(candidate.lexically_normal().c_str(), expected.lexically_normal().c_str()) == 0;
}

std::filesystem::path prepareProfileRoot(const std::filesystem::path& localAppData) {
    const auto root = profileRoot(localAppData);
    if (!validProfileRoot(root, localAppData)) return {};
    std::error_code ec;
    const auto profiles = root / L"profiles";
    if (!safeAbsolute(profiles)) return {};
    std::filesystem::create_directories(profiles, ec);
    if (ec) return {};
    const auto migrationMarker = root / L".cache-index-v1";
    if (std::filesystem::exists(migrationMarker, ec) || ec) return root;
    const auto legacy = localAppData / L"D3DSCache" / L"CacheData";
    const auto oldProfiles = legacy / L"profiles";
    if (!safeAbsolute(oldProfiles) || !std::filesystem::is_directory(oldProfiles, ec) || ec) return root;
    bool complete = true;
    for (std::filesystem::directory_iterator it(oldProfiles, ec), end; !ec && it != end; it.increment(ec)) {
        const auto source = it->path();
        if (source.extension() != L".json") continue;
        auto data = readProfile(source);
        if (!data.is_object() || !data.contains("#theme") || !data["#theme"].is_object()
            || !data.contains("#client") || !data["#client"].is_object()) continue;
        data["#owner"] = profileOwner;
        const auto destination = profiles / source.filename();
        std::error_code destinationError;
        if (!std::filesystem::exists(destination, destinationError)
            && (destinationError || !writeNew(destination, encodeProfileData(data.dump(2))))) complete = false;
    }
    if (ec) complete = false;
    const auto oldMarker = legacy / L"last_profile.txt";
    if (safeAbsolute(oldMarker)) {
        std::ifstream input(oldMarker);
        std::string name;
        if (std::getline(input, name)) {
            if (!name.empty() && name.back() == '\r') name.pop_back();
            if (!name.empty() && name.size() <= 128 && name.find_first_of("/\\:") == std::string::npos
                && std::filesystem::is_regular_file(profiles / (name + ".json"), ec) && !ec)
                writeNew(root / L"last_profile.txt", name);
        }
    }
    // Preserve this marker on purge: deleted imported profiles must not be
    // resurrected from the legacy folder at the next startup.
    if (complete) writeNew(migrationMarker, "D3DConfigImport:v1");
    return root;
}

PurgeResult purgeProfiles(const std::filesystem::path& candidate, const std::filesystem::path& localAppData) {
    PurgeResult result;
    if (!validProfileRoot(candidate, localAppData)) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    const auto profiles = candidate / L"profiles";
    if (!safeAbsolute(profiles)) {
        result.error = std::make_error_code(std::errc::operation_not_permitted);
        return result;
    }
    result.valid = true;
    std::error_code ec;
    if (!std::filesystem::exists(profiles, ec)) { result.error = ec; return result; }
    // Disallow directory rename/replacement while enumerating and deleting.
    FileHandle rootLease, profilesLease;
    if (!plainDirectory(rootLease, candidate) || !plainDirectory(profilesLease, profiles)) {
        result.valid = false;
        result.error = std::make_error_code(std::errc::operation_not_permitted);
        return result;
    }
    std::set<std::string> removedNames;
    for (std::filesystem::directory_iterator it(profiles, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        std::error_code removalError;
        if (path.extension() == L".json" && deleteOwnedProfile(path, removalError)) {
            ++result.removed;
            removedNames.insert(path.stem().string());
        } else {
            ++result.preserved;
            if (removalError && !result.error) result.error = removalError;
        }
    }
    if (ec && !result.error) result.error = ec;
    const auto marker = candidate / L"last_profile.txt";
    if (safeAbsolute(marker)) {
        std::error_code markerError;
        deleteActiveMarker(marker, removedNames, markerError);
        if (markerError && !result.error) result.error = markerError;
    }
    // The root and unrelated files are deliberately retained.
    return result;
}
}
