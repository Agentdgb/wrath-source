#pragma once

// The friend list.
//
// WHY IT IS IN core/ AND NOT IN src/vgui/dt/state/social.hh:
//
//   * src/vgui writes it   — the player list's Status combobox, chat's "Add friend".
//   * src/modules reads it — targeting and the ESP, on threads the GUI does not
//     run on (AimAssist owns a daemon thread that calls findBestTarget at ~1 kHz),
//     up to five figures a second.
//   * src/core/config persists it — and ConfigManager may not include a vgui header.
//
// social.hh cannot be that home: it has no includes of its own and only compiles
// inside vgui/includes.hh's prelude. Routing the query through vgui/bridge.h was
// the other candidate — modules -> vgui/bridge is a real, existing direction
// (esp.cpp, silentaim, triggerbot and thirteen others include it for
// isOverlayVisible) — but bridge.h exists specifically for facts that ONLY exist
// inside the GUI translation unit: the theme accent, the panel rect, whether the
// overlay owns the mouse. A friend list is not one of those. It has to be saved by
// ConfigManager, which is core, so leaving the truth in vgui would force
// core/config -> vgui and invert the dependency the bridge exists to protect.
//
// THREADING: one writer domain (the render thread, which is also the game thread —
// see core/event/GameFramePump.h) and several readers, including AimAssist's own
// attached daemon thread and ConfigManager's autosave thread. Guarded by a
// shared_mutex, but the hot path almost never takes it: empty() and generation()
// are plain atomics, and empty() is true in the overwhelmingly common case, which
// is what makes the per-entity query free when nobody is marked.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dt {

class FriendList {
public:
    static FriendList& get();

    // Membership. Matched on an ASCII-case-folded form of the name (see
    // FriendList.cpp for why), so a differently-cased spelling still matches.
    bool contains(std::string_view name) const;

    // Lock-free. The per-entity cache short-circuits on these two.
    bool          empty()      const noexcept { return count_.load(std::memory_order_relaxed) == 0; }
    std::size_t   size()       const noexcept { return count_.load(std::memory_order_relaxed); }

    // Bumped on every membership change. A caller holding a cached per-entity
    // answer compares this against the generation it cached at and re-derives
    // only on a mismatch, so unmarking somebody takes effect on the next query
    // rather than whenever the cache happens to turn over.
    std::uint32_t generation() const noexcept { return gen_.load(std::memory_order_relaxed); }

    // Returns true when the set actually changed. Marks the config dirty.
    bool set(std::string_view name, bool value);
    bool add(std::string_view name)    { return set(name, true); }
    bool remove(std::string_view name) { return set(name, false); }

    // Display-case names, insertion order. For the GUI and for persistence.
    std::vector<std::string> all() const;

    // The load path. Does NOT mark the config dirty.
    void replaceAll(std::vector<std::string> names);

private:
    FriendList() = default;

    static std::uint64_t fold_hash(std::string_view s) noexcept;
    static bool          equal_folded(std::string_view a, std::string_view b) noexcept;

    void reindex();   // caller holds the write lock

    struct Index { std::uint64_t hash; std::uint32_t slot; };

    mutable std::shared_mutex  mx_;
    std::vector<std::string>   names_;   // display case
    std::vector<Index>         index_;   // sorted by hash — the lookup structure
    std::atomic<std::uint32_t> gen_{ 1 };
    std::atomic<std::size_t>   count_{ 0 };
};

} // namespace dt
