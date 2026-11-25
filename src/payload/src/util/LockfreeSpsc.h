#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace dt {

// Single-producer / single-consumer bounded ring buffer.
// N must be a power of two.
template <class T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    alignas(64) std::atomic<std::size_t> head_{0}; // producer
    alignas(64) std::atomic<std::size_t> tail_{0}; // consumer
    std::array<T, N>                     slots_{};
public:
    bool try_push(T v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        auto h = head_.load(std::memory_order_relaxed);
        auto t = tail_.load(std::memory_order_acquire);
        if (h - t == N) return false;
        slots_[h & (N - 1)] = std::move(v);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        auto t = tail_.load(std::memory_order_relaxed);
        auto h = head_.load(std::memory_order_acquire);
        if (h == t) return false;
        out = std::move(slots_[t & (N - 1)]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    std::size_t size_relaxed() const noexcept {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace dt
