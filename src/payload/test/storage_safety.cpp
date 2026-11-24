#include "core/config/ProfileStorage.h"
#include "platform/ThreadUtil.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <winioctl.h>

namespace vanta::jvm { void detachThisThreadIfAttached() {} }
namespace vanta::platform { HMODULE currentModule() { return GetModuleHandleW(nullptr); } }

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path); file << text;
    require(file.good(), "fixture write failed");
}
HANDLE WINAPI failThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr;
}
void noOperation() {}

bool makeJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link.c_str()); return false; }
    struct Junction {
        DWORD tag; WORD length; WORD reserved;
        WORD substituteOffset, substituteLength, printOffset, printLength;
        WCHAR names[2048];
    } data{};
    const auto print = target.wstring();
    const auto substitute = L"\\??\\" + print;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(WCHAR));
    data.printOffset = data.substituteLength + sizeof(WCHAR);
    data.printLength = static_cast<WORD>(print.size() * sizeof(WCHAR));
    require(substitute.size() + print.size() + 2 < 2048, "junction fixture path too long");
    std::copy(substitute.begin(), substitute.end(), data.names);
    std::copy(print.begin(), print.end(), data.names + substitute.size() + 1);
    data.length = static_cast<WORD>(8 + data.substituteLength + data.printLength + 2 * sizeof(WCHAR));
    DWORD transferred = 0;
    const bool ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
        8 + data.length, nullptr, 0, &transferred, nullptr) != FALSE;
    CloseHandle(directory);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}

int main() {
    try {
        const auto fixture = std::filesystem::current_path()
            / ("storage-safety-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        require(std::filesystem::create_directory(fixture), "fixture must be a new directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        } cleanup{fixture};
        const auto appData = fixture / "LocalAppData";
        std::filesystem::create_directory(appData);
        const auto legacy = appData / "D3DSCache/CacheData/profiles";
        std::filesystem::create_directories(legacy);
        const nlohmann::json previous{{"#theme", nlohmann::json::object()},
            {"#client", {{"scale", 1.2}}}, {"payload", "original"}};
        write(legacy / "previous.json", previous.dump());
        write(legacy.parent_path() / "last_profile.txt", "previous");
        const auto root = vanta::storage::prepareProfileRoot(appData);
        require(root == vanta::storage::profileRoot(appData), "storage is not a dedicated directory");
        require(std::filesystem::exists(root / "profiles/previous.json"), "legacy profile was not copied");
        require(std::filesystem::exists(legacy / "previous.json"), "migration deleted original profile");
        write(root / "profiles/previous.json", vanta::storage::encodeProfileData(nlohmann::json{{"#owner", vanta::storage::profileOwnerToken()},
            {"payload", "newer"}}.dump()));
        vanta::storage::prepareProfileRoot(appData);
        const auto updated = vanta::storage::readProfile(root / "profiles/previous.json");
        require(updated["payload"] == "newer", "migration overwrote a newer profile");
        write(root / "notes.txt", "unrelated root file");
        write(root / "profiles/unrelated.json", "{\"personal\": true}");
        write(root / "profiles/broken.json", "not JSON");
        std::filesystem::create_directory(root / "profiles/subdirectory");
        write(root / "profiles/subdirectory/keep.txt", "unrelated nested data");
        const auto foreign = fixture / "foreign";
        std::filesystem::create_directory(foreign);
        write(foreign / "keep.txt", "foreign data");
        for (const auto& unsafe : {std::filesystem::path{}, std::filesystem::path{"."},
                fixture.root_path(), foreign, appData, root / ".."}) {
            const auto result = vanta::storage::purgeProfiles(unsafe, appData);
            require(!result.valid && result.removed == 0, "unsafe path accepted");
        }
        require(vanta::storage::profileRoot({}).empty()
            && vanta::storage::profileRoot(".").empty(), "failed AppData resolution got a relative fallback");
        require(std::filesystem::exists(foreign / "keep.txt"), "foreign data removed");
        HANDLE held = CreateFileW((root / "profiles/previous.json").c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(held != INVALID_HANDLE_VALUE, "could not lock fixture profile");
        const auto blocked = vanta::storage::purgeProfiles(root, appData);
        CloseHandle(held);
        require(blocked.error && std::filesystem::exists(root / "profiles/previous.json"),
            "locked profile deletion failure not reported");
        const auto purged = vanta::storage::purgeProfiles(root, appData);
        require(purged.valid && !purged.error && purged.removed == 1, "owned profile purge failed");
        require(!std::filesystem::exists(root / "profiles/previous.json"), "owned profile remains");
        require(std::filesystem::exists(root / "notes.txt")
            && std::filesystem::exists(root / "profiles/unrelated.json")
            && std::filesystem::exists(root / "profiles/broken.json")
            && std::filesystem::exists(root / "profiles/subdirectory/keep.txt"), "unrelated data removed");
        require(!std::filesystem::exists(root / "last_profile.txt"), "active marker not retired with its profile");
        require(std::filesystem::exists(legacy / "previous.json"), "purge touched legacy data");
        vanta::storage::prepareProfileRoot(appData);
        require(!std::filesystem::exists(root / "profiles/previous.json"), "migration resurrected a purged profile");
        const auto link = root / "profiles-link";
        if (CreateSymbolicLinkW(link.c_str(), foreign.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
            || makeJunction(link, foreign)) {
            const auto saved = root / "saved-profiles";
            std::filesystem::rename(root / "profiles", saved);
            std::filesystem::rename(link, root / "profiles");
            write(foreign / "owned.json", vanta::storage::encodeProfileData(nlohmann::json{{"#owner", vanta::storage::profileOwnerToken()}}.dump()));
            const auto redirected = vanta::storage::purgeProfiles(root, appData);
            require(!redirected.valid && std::filesystem::exists(foreign / "owned.json"),
                "purge followed a directory symlink");
            std::filesystem::remove(root / "profiles");
            std::filesystem::rename(saved, root / "profiles");
            std::cout << "Directory redirection rejected; target data preserved\n";
        } else {
            std::cout << "Creating a redirection fixture is not permitted; checking an existing junction read-only\n";
            wchar_t publicDirectory[32768]{};
            const auto length = GetEnvironmentVariableW(L"PUBLIC", publicDirectory, 32768);
            require(length > 0 && length < 32768, "PUBLIC directory unavailable for read-only junction check");
            const auto alias = std::filesystem::path(publicDirectory).parent_path() / L"All Users";
            const auto attributes = GetFileAttributesW(alias.c_str());
            require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "existing Windows junction unavailable");
            // No file enumeration/deletion: rejection must happen at validation.
            const auto redirected = vanta::storage::purgeProfiles(alias / vanta::storage::profileRoot(appData).filename(), alias);
            require(!redirected.valid && redirected.removed == 0, "existing junction ancestor accepted");
            std::cout << "Existing Windows junction rejected without touching its target\n";
        }
        require(!vanta::platform::spawnReaper(&noOperation, &failThread), "failed thread reported as created");
        require(GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "thread creation error lost");
        require(!vanta::platform::spawnReaper(nullptr), "null reaper accepted");
        std::cout << "Storage safety: unsafe paths rejected, legacy originals retained, unrelated files preserved, errors reported\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
