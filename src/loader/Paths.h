#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace wrath::loader {

// Directory containing the running executable.
std::wstring exe_dir();

// Deterministic payload resolution, in order:
//   1. --payload <path>  (command-line override)
//   2. <exe_dir>\display_service64.dll
//   3. <exe_dir>\payload.dll
// Returns an empty string when no candidate exists.
std::wstring resolve_payload(const std::wstring& override_path = {});

// Reads the resolved payload into memory. Returns empty on failure.
std::vector<uint8_t> read_payload(const std::wstring& path);

} // namespace wrath::loader
