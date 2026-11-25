#pragma once
#include <atomic>

namespace dt {
// Once teardown leaves a callback or hook reachable, caches and the image
// must remain alive. A timeout is a failure, never permission to free memory.
inline std::atomic<bool> g_unloadSafe{true};
inline void blockUnload() noexcept { g_unloadSafe.store(false, std::memory_order_release); }
inline bool safeToUnload() noexcept { return g_unloadSafe.load(std::memory_order_acquire); }
}
