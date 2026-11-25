#pragma once
#include <span>

namespace dt::mappings {

// Chicken-and-egg break: VersionDetect needs SharedConstants BEFORE
// MappingRegistry is bound, so it probes a small candidate list of runtime
// class names. Post-pivot: Fabric-only, so the list is intermediary +
// dev-env only — no Vanilla obf shifts to track.
std::span<const char* const> bootstrapSharedConstantsCandidates();

} // namespace dt::mappings
