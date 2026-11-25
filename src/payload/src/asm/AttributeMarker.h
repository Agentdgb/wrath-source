#pragma once
#include <cstdint>
#include <string_view>

namespace dt::asm_ {

// Custom "Dt$Marker" class attribute. Injected on transform; on
// re-transform we skip classes whose marker matches the current agent build.
constexpr std::string_view kMarkerAttribute = "Dt$Marker";

struct MarkerPayload {
    std::uint32_t agentBuildHash;
    std::uint32_t hookIdBitset[2]; // which hooks we spliced (up to 64)
};

} // namespace dt::asm_
