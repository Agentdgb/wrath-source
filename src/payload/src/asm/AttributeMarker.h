#pragma once
#include <cstdint>
#include <string_view>

namespace vanta::asm_ {

// Custom "Vanta$Marker" class attribute. Injected on transform; on
// re-transform we skip classes whose marker matches the current agent build.
constexpr std::string_view kMarkerAttribute = "Vanta$Marker";

struct MarkerPayload {
    std::uint32_t agentBuildHash;
    std::uint32_t hookIdBitset[2]; // which hooks we spliced (up to 64)
};

} // namespace vanta::asm_
