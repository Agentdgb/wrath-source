#pragma once
#include "core/event/Event.h"
#include "core/event/Priority.h"
#include "core/ClientState.h"
#include "util/Log.h"

#include <ankerl/unordered_dense.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vanta {

template <typename T>
consteval uint64_t typeHash() noexcept {
    constexpr std::string_view sig = __FUNCSIG__;
    uint64_t hash = 14695981039346656037ULL;
    for (char c : sig) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

class EventBus;

struct IListenerToken {
    virtual ~IListenerToken() = default;
};

// A single subscription slot. Kept alive by the EventBus and pinned by the
// caller's unique_ptr<IListenerToken> — when the token drops, the slot is
// marked dead and compacted on next dispatch.
struct EventBusSlot {
    std::function<void(void*)> fn;
    Priority                   prio  = Priority::Normal;
    std::atomic<bool>          alive{true};
};

class EventBus {
public:
    static EventBus& get();

    template <class E, class Fn>
    [[nodiscard]] std::unique_ptr<IListenerToken> subscribe(Fn&& fn, Priority p = Priority::Normal) {
        static_assert(std::is_base_of_v<Event, E>, "E must derive from vanta::Event");
        auto& bucket = bucketFor(typeHash<E>());
        std::unique_lock lk(bucket.mx);

        auto slot   = std::make_shared<EventBusSlot>();
        slot->prio  = p;
        slot->alive = true;
        if constexpr (std::is_invocable_v<Fn, E&>) {
            slot->fn = [f = std::forward<Fn>(fn)](void* p) mutable { f(*static_cast<E*>(p)); };
        } else if constexpr (std::is_invocable_v<Fn>) {
            slot->fn = [f = std::forward<Fn>(fn)](void*) mutable { f(); };
        } else {
            static_assert(std::is_invocable_v<Fn, E&>, "Handler must be callable with (E&) or ()");
        }
        auto next = std::make_shared<Slots>(*bucket.snapshot.load());
        std::erase_if(*next, [](const auto& s) { return !s->alive.load(); });
        next->push_back(slot);
        std::stable_sort(next->begin(), next->end(), [](const auto& a, const auto& b) {
            if (a->prio == Priority::Monitor) return false;
            if (b->prio == Priority::Monitor) return true;
            return static_cast<int>(a->prio) > static_cast<int>(b->prio);
        });
        bucket.snapshot.store(std::move(next));

        struct TokenImpl : IListenerToken {
            std::weak_ptr<EventBusSlot> slot;
            ~TokenImpl() override {
                std::lock_guard dispatch(clientStateMutex());
                if (auto s = slot.lock()) s->alive.store(false);
            }
        };
        auto t  = std::make_unique<TokenImpl>();
        t->slot = slot;
        return t;
    }

    // Post an event to all registered listeners. Returns whether the event
    // survived (for Cancellable events, false when a listener cancelled).
    template <class E>
    bool post(E& evt) {
        static_assert(std::is_base_of_v<Event, E>, "E must derive from vanta::Event");
        auto& bucket = bucketFor(typeHash<E>());
        const auto snapshot = bucket.snapshot.load();
        std::lock_guard dispatch(clientStateMutex());
        if (stopped_) {
            if constexpr (std::is_base_of_v<Cancellable, E>) return !evt.cancelled;
            return true;
        }
        for (auto& s : *snapshot) {
            if (!s->alive.load()) continue;
            if constexpr (std::is_base_of_v<Cancellable, E>) {
                if (evt.cancelled && s->prio != Priority::Monitor) continue;
            }
            try {
                s->fn(&evt);
            } catch (const std::exception& ex) {
                vanta::log::write(vanta::log::Level::Error, "event", std::string("Listener exception: ") + ex.what());
            } catch (...) {
                vanta::log::write(vanta::log::Level::Error, "event", "Unknown listener exception");
            }
        }
        if constexpr (std::is_base_of_v<Cancellable, E>) return !evt.cancelled;
        return true;
    }

    void stop() { std::lock_guard dispatch(clientStateMutex()); stopped_ = true; }
    void start() { std::lock_guard dispatch(clientStateMutex()); stopped_ = false; }

private:
    using Slots = std::vector<std::shared_ptr<EventBusSlot>>;
    struct Bucket {
        std::mutex mx;
        std::atomic<std::shared_ptr<const Slots>> snapshot{std::make_shared<const Slots>()};
    };
    bool stopped_ = false; // guarded by clientStateMutex

    Bucket& bucketFor(uint64_t id) {
        {
            std::shared_lock lk(bucketMx_);
            if (auto it = buckets_.find(id); it != buckets_.end()) return *it->second;
        }
        std::unique_lock lk(bucketMx_);
        auto& p = buckets_[id];
        if (!p) p = std::make_unique<Bucket>();
        return *p;
    }

    mutable std::shared_mutex                                      bucketMx_;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<Bucket>> buckets_;
};

} // namespace vanta
