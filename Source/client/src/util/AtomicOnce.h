#pragma once
#include <atomic>
#include <cstdint>
#include <utility>

namespace vanta {

// Idempotent once-init: multiple threads race; the winner runs Fn to completion,
// the losers spin until Done. Safe to call from any entry point (Agent_OnLoad,
// Agent_OnAttach, VantaInit) with no ordering guarantee between them.
class AtomicOnce {
    enum : std::uint8_t { Idle = 0, Running = 1, Done = 2, Failed = 3 };
    std::atomic<std::uint8_t> state_{Idle};
public:
    template <class Fn>
    bool run(Fn&& fn) noexcept(noexcept(fn())) {
        std::uint8_t expected = Idle;
        if (state_.compare_exchange_strong(expected, Running, std::memory_order_acq_rel)) {
            try {
                std::forward<Fn>(fn)();
                state_.store(Done, std::memory_order_release);
                state_.notify_all();
                return true;
            } catch (...) {
                state_.store(Failed, std::memory_order_release);
                state_.notify_all();
                throw;
            }
        }
        while (true) {
            auto s = state_.load(std::memory_order_acquire);
            if (s == Done)   return false;
            if (s == Failed) return false;
            state_.wait(s, std::memory_order_acquire);
        }
    }

    bool done() const noexcept   { return state_.load(std::memory_order_acquire) == Done; }
    bool failed() const noexcept { return state_.load(std::memory_order_acquire) == Failed; }
};

} // namespace vanta
