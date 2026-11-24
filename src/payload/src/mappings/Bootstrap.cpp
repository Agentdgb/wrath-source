#include "mappings/Bootstrap.h"

namespace vanta::mappings {

namespace {

// Only two candidates now — Fabric-only pivot. Vanilla obf shift table dropped.
constexpr const char* kSharedConstantsCandidates[] = {
    "net/minecraft/class_155",       // Fabric intermediary (stable across 1.21.x)
    "net/minecraft/SharedConstants", // Dev env (fabric-loom Mojang mappings)
};

} // namespace

std::span<const char* const> bootstrapSharedConstantsCandidates() {
    return {kSharedConstantsCandidates,
            sizeof(kSharedConstantsCandidates) / sizeof(*kSharedConstantsCandidates)};
}

} // namespace vanta::mappings
