#pragma once
#include <utility>

namespace dt {

template <class Fn>
class ScopeGuard {
    Fn   fn_;
    bool live_ = true;
public:
    explicit ScopeGuard(Fn fn) noexcept : fn_(std::move(fn)) {}
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&& o) noexcept : fn_(std::move(o.fn_)), live_(o.live_) { o.live_ = false; }
    ~ScopeGuard() { if (live_) fn_(); }
    void release() noexcept { live_ = false; }
};

template <class Fn>
ScopeGuard(Fn) -> ScopeGuard<Fn>;

} // namespace dt

#define DT_CAT2(a, b) a##b
#define DT_CAT(a, b)  DT_CAT2(a, b)
#define DT_DEFER(...) ::dt::ScopeGuard DT_CAT(_scope_, __LINE__)([&](){ __VA_ARGS__; })
