#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

namespace dt {

// Intrusive multi-producer / single-consumer stack. Producers push; the single
// consumer drains all at once (LIFO order — safe when the queue holds unordered
// side effects like log lines or notifications).
template <class Node>
class MpscStack {
    std::atomic<Node*> top_{nullptr};
public:
    void push(Node* n) noexcept {
        Node* cur = top_.load(std::memory_order_relaxed);
        do { n->next = cur; }
        while (!top_.compare_exchange_weak(cur, n, std::memory_order_release, std::memory_order_relaxed));
    }
    Node* drain() noexcept { return top_.exchange(nullptr, std::memory_order_acquire); }
    bool  empty() const noexcept { return top_.load(std::memory_order_relaxed) == nullptr; }
};

} // namespace dt
