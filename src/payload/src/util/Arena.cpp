#include "util/Arena.h"

#include <cstdlib>

namespace dt {

void* Arena::alloc(std::size_t bytes, std::size_t align) noexcept {
    std::size_t off  = off_;
    std::size_t mask = align - 1;
    off = (off + mask) & ~mask;
    if (off + bytes > cap_) return nullptr;
    off_ = off + bytes;
    return base_ + off;
}

namespace {

constexpr std::size_t kThreadArenaBytes = 1 << 20; // 1 MiB per thread

static std::byte* tl_buf_ = nullptr;
static Arena*     tl_arena_ = nullptr;

struct ThreadArenaGuard {
    Arena arena;
    ThreadArenaGuard() {
        tl_buf_   = static_cast<std::byte*>(std::malloc(kThreadArenaBytes));
        arena     = Arena(tl_buf_, kThreadArenaBytes);
        tl_arena_ = &arena;
    }
    ~ThreadArenaGuard() {
        tl_arena_ = nullptr;
        std::free(tl_buf_);
        tl_buf_ = nullptr;
    }
};

} // namespace

Arena& threadArena() {
    static ThreadArenaGuard g;
    return *tl_arena_;
}

} // namespace dt
