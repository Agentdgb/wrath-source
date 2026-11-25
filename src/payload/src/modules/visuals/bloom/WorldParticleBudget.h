#pragma once
#include <array>
#include <cmath>
#include <cstddef>

namespace dt::modules::bloom::world {
inline constexpr int maxLines = 512, maxQuads = 128, maxSprites = 256, maxTrailSprites = 128;
// Fixed storage and a hard query budget keep mass respawns from stalling a frame.
class TerrainCache {
    struct Entry { int x = 0, z = 0, height = 0; double expires = 0; bool valid = false; };
    std::array<Entry, 128> entries_{};
    int remaining_ = 0;
    double now_ = 0;
public:
    static constexpr int queriesPerFrame = 4;
    void beginFrame(double now) { now_ = now; remaining_ = queriesPerFrame; }
    void reset() { entries_ = {}; remaining_ = 0; }
    template<class Query> int get(int x, int z, int fallback, Query query) {
        const auto hash = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(z) * 19349663u);
        auto &entry = entries_[hash % entries_.size()];
        if (entry.valid && entry.x == x && entry.z == z && now_ < entry.expires)
            return entry.height;
        if (remaining_ <= 0)
            return fallback;
        --remaining_;
        const int height = query(x, z);
        entry = {x, z, height, now_ + .5, true};
        return height;
    }
};
} // namespace dt::modules::bloom::world
