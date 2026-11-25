#pragma once
#include <chrono>

namespace dt::util {
class TimeBudget {
    std::chrono::steady_clock::time_point deadline_;
public:
    explicit TimeBudget(std::chrono::microseconds duration)
        : deadline_(std::chrono::steady_clock::now() + duration) {}
    bool expired() const noexcept { return std::chrono::steady_clock::now() >= deadline_; }
};
}
