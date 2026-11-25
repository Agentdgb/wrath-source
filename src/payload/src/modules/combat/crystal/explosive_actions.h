#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dt::modules::explosive_actions {

struct Cadence { std::uint64_t actionMs, cycleMs; };

constexpr Cadence cadence(int preset) {
    if (preset == 0) return {200, 500};
    if (preset == 2) return {75, 100};
    return {125, 250};
}

struct ActionClock {
    std::uint64_t next = 0;
    bool ready(std::uint64_t now) const { return now >= next; }
    void mark(std::uint64_t now, int preset) { next = now + cadence(preset).actionMs; }
    void clear() { next = 0; }
};

enum class Action { CrystalPlace, AnchorPlace, AnchorCharge, AnchorDetonate, Shield };

inline bool matchesItem(Action action, std::string_view key, bool empty,
                        int charges = 0, bool shieldMatches = false) {
    if (action == Action::AnchorDetonate) {
        return charges > 0 && charges <= 4 && !key.ends_with(".respawn_anchor")
            && (empty || !key.ends_with(".glowstone") || charges == 4);
    }
    if (empty) return false;
    switch (action) {
        case Action::CrystalPlace: return key.ends_with(".end_crystal");
        case Action::AnchorPlace: return key.ends_with(".respawn_anchor");
        case Action::AnchorCharge: return charges == 0 && key.ends_with(".glowstone");
        case Action::Shield: return shieldMatches;
        default: return false;
    }
}

struct TickGate {
    std::optional<unsigned> last;
    bool available(unsigned tick) const { return !last || *last != tick; }
    bool commit(unsigned tick) {
        if (!available(tick)) return false;
        last = tick;
        return true;
    }
};

class SlotRestore {
    int original = -1, changed = -1;
public:
    bool active() const { return original >= 0; }
    void clear() { original = changed = -1; }
    bool observe(int selected) {
        if (!active() || selected == changed) return false;
        clear();
        return true;
    }
    void record(int selected, int destination) {
        observe(selected);
        if (selected < 0 || selected > 8 || destination < 0 || destination > 8 || selected == destination) return;
        if (!active()) original = selected;
        changed = destination;
    }
    std::optional<int> target(int selected) const {
        return active() && selected == changed ? std::optional<int>{original} : std::nullopt;
    }
};

enum class ScanDecision { Skip, Accept, Yield };

// A bounded search resumes at the first unexamined candidate. Yield keeps the
// current candidate for the next pass when an expensive-work budget runs out.
template<class Range, class Evaluate>
std::optional<typename Range::value_type> scan(const Range& candidates, std::size_t& cursor,
                                              std::size_t budget, Evaluate&& evaluate) {
    if (cursor >= candidates.size()) cursor = 0;
    for (std::size_t checked = 0; checked < budget && cursor < candidates.size(); ++checked) {
        const auto& candidate = candidates[cursor];
        const auto result = evaluate(candidate);
        if (result == ScanDecision::Yield) break;
        ++cursor;
        if (result == ScanDecision::Accept) return candidate;
    }
    return std::nullopt;
}

} // namespace dt::modules::explosive_actions
