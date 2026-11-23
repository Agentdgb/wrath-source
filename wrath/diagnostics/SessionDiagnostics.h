#pragma once
#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace wrath::diagnostics {

inline unsigned long long fileTimeValue(FILETIME value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

// Explicit diagnostic files, containing metadata only: no tokens, HWID,
// profile contents or process memory contents. Failure never stops the game.
inline bool writeRecord(const wchar_t* kind, DWORD pid, HANDLE process,
    std::string_view fields, const std::filesystem::path& testDirectory = {}) noexcept {
    try {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
        GetSystemTimeAsFileTime(&now);
        std::filesystem::path directory = testDirectory;
        if (directory.empty()) {
            // En sesion de juego (produccion/stealth), no escribir ningun archivo de diagnostico a disco
            // para no dejar carpetas 'WrathClient' ni registros forenses USN/MFT.
            return true;
        }
        if (!directory.is_absolute()) return false;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        const auto destination = directory / (std::wstring(kind) + L"-" + std::to_wstring(pid) + L".json");
        const auto temporary = std::filesystem::path(destination.wstring() + L".tmp");
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << "{\"version\":1,\"pid\":" << pid
                << ",\"process_start\":" << fileTimeValue(created)
                << ",\"recorded_at\":" << fileTimeValue(now) << fields << "}\n";
            stream.close();
            if (!stream) return false;
        }
        return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    } catch (...) { return false; }
}

enum class LoadMethod { LoadLibrary, TxfImage, ModuleStomp, ManualMap };

inline void recordLoadMethod(DWORD pid, LoadMethod method) noexcept {
    const char* name = "unknown";
    switch (method) {
        case LoadMethod::LoadLibrary: name = "LoadLibrary"; break;
        case LoadMethod::TxfImage: name = "TxF image mapping"; break;
        case LoadMethod::ModuleStomp: name = "Module stomp"; break;
        case LoadMethod::ManualMap: name = "Manual mapping"; break;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return;
    try { writeRecord(L"load", pid, process, std::string(",\"method\":\"") + name + "\""); }
    catch (...) {}
    CloseHandle(process);
}

inline void recordModule(HMODULE module, const std::filesystem::path& testDirectory = {}) noexcept {
    if (!module) return;
    try {
        MEMORY_BASIC_INFORMATION first{};
        if (!VirtualQuery(module, &first, sizeof(first)) || !first.AllocationBase) return;
        const auto allocation = reinterpret_cast<std::uintptr_t>(first.AllocationBase);
        std::uintptr_t cursor = allocation;
        bool complete = false;
        // Describe the allocation using page metadata; do not read its contents.
        for (unsigned region = 0; region < 65536; ++region) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory))) return;
            if (memory.AllocationBase != first.AllocationBase) { complete = true; break; }
            const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (end <= cursor || end - allocation > 1024ULL * 1024 * 1024) return;
            cursor = end;
        }
        if (!complete || cursor <= allocation) return;
        HMODULE registered = nullptr;
        const bool known = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(module), &registered) && registered == module;
        writeRecord(L"module", GetCurrentProcessId(), GetCurrentProcess(),
            ",\"module_base\":" + std::to_string(reinterpret_cast<std::uintptr_t>(module))
            + ",\"allocation_base\":" + std::to_string(allocation)
            + ",\"allocation_size\":" + std::to_string(cursor - allocation)
            + ",\"registered_at_start\":" + (known ? "true" : "false"), testDirectory);
    } catch (...) {}
}

inline void recordUnloadRequest(const std::filesystem::path& testDirectory = {}) noexcept {
    writeRecord(L"unload", GetCurrentProcessId(), GetCurrentProcess(),
        ",\"status\":\"requested\",\"preserve_data\":true", testDirectory);
}

// ─────────────────────────────────────────────────────────────────────────────
// Session cleanup — called at the end of shutdown() to erase all diagnostic
// artefacts written during this session before anyone can read them.
//
// Each JSON file is first overwritten with zero bytes (best-effort to defeat
// filesystem-level recovery), then deleted. If the directory becomes empty it
// is also removed. Failures are silently swallowed; the host process is dying.
// ─────────────────────────────────────────────────────────────────────────────
inline void purgeSessionRecords(DWORD pid,
    const std::filesystem::path& testDirectory = {}) noexcept {
    try {
        // ── Resolve diagnostics directory ──────────────────────────────────
        std::filesystem::path dir = testDirectory;
        if (dir.empty()) {
            PWSTR local = nullptr;
            if (FAILED(SHGetKnownFolderPath(
                    FOLDERID_LocalAppData, 0, nullptr, &local))) return;
            try {
                dir = std::filesystem::path(local)
                    / L"WrathClient" / L"diagnostics";
            } catch (...) {
                CoTaskMemFree(local);
                return;
            }
            CoTaskMemFree(local);
        }
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || ec) return;

        // ── Collect files whose stem ends with "-<pid>" ────────────────────
        const std::wstring pidSuffix = L"-" + std::to_wstring(pid);
        std::vector<std::filesystem::path> targets;
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) continue;
            const auto stem = entry.path().stem().wstring();
            if (stem.size() > pidSuffix.size() &&
                stem.substr(stem.size() - pidSuffix.size()) == pidSuffix) {
                targets.push_back(entry.path());
            }
        }

        // ── Wipe then delete each target ───────────────────────────────────
        for (const auto& p : targets) {
            std::error_code ec2;
            const auto sz = std::filesystem::file_size(p, ec2);
            if (!ec2 && sz > 0) {
                // Overwrite with zeros before deletion to impede recovery.
                std::ofstream f(p, std::ios::binary | std::ios::trunc);
                if (f) {
                    const std::vector<char> zeros(static_cast<size_t>(sz), '\0');
                    f.write(zeros.data(), static_cast<std::streamsize>(sz));
                    f.flush();
                }
            }
            std::filesystem::remove(p, ec2);
        }

        // ── Remove the directory if now empty ──────────────────────────────
        bool empty = true;
        for (auto& _ : std::filesystem::directory_iterator(dir, ec)) {
            (void)_;
            if (!ec) { empty = false; break; }
        }
        if (empty && !ec) {
            std::filesystem::remove(dir, ec);
            // Also remove parent "WrathClient" if empty.
            auto parent = dir.parent_path();
            bool parentEmpty = true;
            for (auto& _ : std::filesystem::directory_iterator(parent, ec)) {
                (void)_;
                if (!ec) { parentEmpty = false; break; }
            }
            if (parentEmpty && !ec)
                std::filesystem::remove(parent, ec);
        }
    } catch (...) {}
}

} // namespace wrath::diagnostics
