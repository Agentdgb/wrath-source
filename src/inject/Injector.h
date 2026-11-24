#pragma once

#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wrath::inject {

enum class Strategy : uint8_t {
    Auto,
    MapPrivate,
    MapSectionImage,
    MapStomp,
    RunThread,
    RunHijack,
};

struct Options {
    bool unlink = true;
    bool wipe_headers = true;
    bool reprotect = true;
    bool zero_scratch = true;
    bool hide_thread = true;
};

struct Attempt {
    Strategy strategy = Strategy::Auto;
    bool ok = false;
    std::wstring detail;
    uint64_t elapsed_ms = 0;
};

struct Result {
    bool ok = false;
    Strategy chosen = Strategy::Auto;
    DWORD target_pid = 0;
    std::wstring detail;
    std::vector<Attempt> attempts;
};

// Main entry point. Tries strategies in order (Auto walks the table).
Result inject(HANDLE hProcess, DWORD pid,
              std::span<const uint8_t> payload,
              const Options& options);

const char* strategy_name(Strategy s);
bool parse_strategy(const std::string& name, Strategy* out);

} // namespace wrath::inject
