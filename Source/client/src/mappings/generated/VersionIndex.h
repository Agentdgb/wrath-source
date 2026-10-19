#pragma once
#include "mappings/generated/GenTable.h"

#include <span>
#include <string_view>

namespace vanta::mappings::gen {

// Runtime lookup entry point implemented by generated/mappings_index.cpp.
// Returns nullptr when the version string is not one this DLL ships tables
// for; callers then fall back to defaultVersionTable().
const VersionTable* findVersionTable(std::string_view mcVersion) noexcept;

// The default table used when detection fails or the version is unknown —
// currently v_1_21_11 (kept the newest so a new point release without
// regenerated tables at least tries the freshest known layout).
const VersionTable* defaultVersionTable() noexcept;

// Diagnostic dump: enumerates every shipped version. Used by VLOG dumps and
// the "-list-mappings" debug switch.
struct IndexRow {
    std::string_view      mcVersion;
    const VersionTable*   table;
};
std::span<const IndexRow> allVersionTables() noexcept;

} // namespace vanta::mappings::gen
