#pragma once

#include <cstdint>

namespace wrath::bypass {

// Named-object prefixes and heartbeat identity, centralized.
// These are the only strings that identify our objects in the system.
inline constexpr wchar_t kSentinelPrefix[]  = L"Local\\wrath_sentinel_";
inline constexpr wchar_t kHeartbeatPrefix[] = L"Local\\wrath_hb_";
inline constexpr uint32_t kHeartbeatMagic   = 0x57525448u; // 'WRTH'
inline constexpr uint32_t kHeartbeatVersion = 2u;

} // namespace wrath::bypass
