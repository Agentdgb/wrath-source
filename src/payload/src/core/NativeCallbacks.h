#pragma once
#include <atomic>
#include <chrono>
#include <thread>

namespace dt {
inline std::atomic<unsigned> g_nativeCallbacks{0};
struct NativeCallback {
    NativeCallback() { g_nativeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~NativeCallback() { g_nativeCallbacks.fetch_sub(1, std::memory_order_release); }
};
inline bool drainNativeCallbacks() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_nativeCallbacks.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return g_nativeCallbacks.load(std::memory_order_acquire) == 0;
}
}
