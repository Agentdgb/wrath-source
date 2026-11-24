#pragma once

#include <cstdint>

namespace wrath::bypass {

// Named-object prefixes and heartbeat identity, centralized.
// These are the only strings that identify our objects in the system.
inline constexpr wchar_t kSentinelPrefix[]  = L"Local\\DisplayTuner.Sentinel.";
inline constexpr wchar_t kHeartbeatPrefix[] = L"Local\\DisplayTuner.Hb.";
inline constexpr uint32_t kHeartbeatMagic   = 0x4454554Eu; // 'DTUN' (DisplayTuner)
inline constexpr uint32_t kHeartbeatVersion = 3u;

} // namespace wrath::bypass
