#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace dt {

// Bump-allocator arena. Not thread-safe on its own — one instance per thread
// (see threadArena()). Reset per class transform to keep peak memory bounded.
class Arena {
    std::byte*  base_ = nullptr;
    std::size_t cap_  = 0;
    std::size_t off_  = 0;
public:
    Arena() = default;
    Arena(std::byte* base, std::size_t cap) noexcept : base_(base), cap_(cap) {}

    void*       alloc(std::size_t bytes, std::size_t align) noexcept;
    void        reset() noexcept { off_ = 0; }
    std::size_t used()  const noexcept { return off_; }
    std::size_t capacity() const noexcept { return cap_; }

    template <class T, class... A>
    T* make(A&&... a) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena objects are freed en-masse; must be trivially destructible.");
        auto* p = static_cast<T*>(alloc(sizeof(T), alignof(T)));
        if (!p) return nullptr;
        return new (p) T(std::forward<A>(a)...);
    }

    template <class T>
    T* makeArray(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T>);
        return static_cast<T*>(alloc(sizeof(T) * n, alignof(T)));
    }
};

Arena& threadArena();

} // namespace dt
