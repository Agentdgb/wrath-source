#pragma once
#include <cmath>
#include <algorithm>
#include <string>
#include <string_view>
#include "modules/shared/NativeAccess.h"

namespace vanta::modules::crystal_geom {

using native::Vec;

constexpr double kDeg = 57.2957795130823;
constexpr double kRad = 0.0174532925199433;

struct Block {
    int x = 0;
    int y = 0;
    int z = 0;
    bool operator==(const Block&) const = default;
};

struct Box6 {
    double minX = 0;
    double minY = 0;
    double minZ = 0;
    double maxX = 0;
    double maxY = 0;
    double maxZ = 0;
};

// Entity.getRotationVector: yaw/pitch in degrees -> (-sin y cos p, -sin p, cos y cos p).
inline Vec dirOf(float yaw, float pitch) {
    const double y = yaw * kRad;
    const double p = pitch * kRad;
    return {-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
}

inline Vec add(Vec a, Vec b, double s = 1.0) {
    return {a.x + b.x * s, a.y + b.y * s, a.z + b.z * s};
}

// Slab test. tEntry is 0 when the origin is inside the box (Grim does the same).
inline bool rayEntersBox(Vec o, Vec d, const Box6& b, double maxT, double& tEntry) {
    double t0 = 0.0;
    double t1 = maxT;
    const double os[3] = {o.x, o.y, o.z};
    const double ds[3] = {d.x, d.y, d.z};
    const double mn[3] = {b.minX, b.minY, b.minZ};
    const double mx[3] = {b.maxX, b.maxY, b.maxZ};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-9) {
            if (os[i] < mn[i] || os[i] > mx[i]) return false;
            continue;
        }
        double lo = (mn[i] - os[i]) / ds[i];
        double hi = (mx[i] - os[i]) / ds[i];
        if (lo > hi) std::swap(lo, hi);
        t0 = std::max(t0, lo);
        t1 = std::min(t1, hi);
        if (t0 > t1) return false;
    }
    tEntry = t0;
    return true;
}

inline Box6 cellBox(Block b) {
    return {double(b.x), double(b.y), double(b.z), double(b.x + 1), double(b.y + 1), double(b.z + 1)};
}

inline bool anchorInReach(Block b, Vec eye, double reach) {
    if (!std::isfinite(reach) || reach <= 0.05) return false;
    const double dx = eye.x - std::clamp(eye.x, double(b.x), double(b.x + 1));
    const double dy = eye.y - std::clamp(eye.y, double(b.y), double(b.y + 1));
    const double dz = eye.z - std::clamp(eye.z, double(b.z), double(b.z + 1));
    return (dx * dx + dy * dy + dz * dz) <= ((reach - 0.05) * (reach - 0.05));
}

// Geometric coverage of the lower body, now and one movement step ahead.
// Ranks shielding positions; does not replace blast calculations.
inline int shieldCoverage(Block anchor, Block spot, Vec feet, Vec velocity) {
    const Vec origin{anchor.x + 0.5, anchor.y + 0.5, anchor.z + 0.5};
    Box6 box = cellBox(spot);
    box.minX += 0.02; box.minY += 0.02; box.minZ += 0.02;
    box.maxX -= 0.02; box.maxY -= 0.02; box.maxZ -= 0.02;
    int score = 3;
    for (int step = 0; step < 2; ++step) {
        int hits = 0;
        for (double height : {0.2, 0.6, 1.0}) {
            Vec target = add(feet, velocity, double(step));
            target.y += height;
            const Vec delta = add(target, origin, -1.0);
            double entry = 0.0;
            if (rayEntersBox(origin, delta, box, 0.98, entry) && entry > 0.01) {
                ++hits;
            }
        }
        score = std::min(score, hits);
    }
    return score;
}

// Item.getTranslationKey is "block.minecraft.<x>" for every BlockItem (the
// empty hand included: Items.AIR is one) and "item.minecraft.<x>" otherwise.
inline bool blockItem(const std::string& key) {
    return key.starts_with("block.");
}

inline bool isGlowstone(const std::string& k) {
    return k.ends_with(".glowstone");
}

inline bool isObsidian(const std::string& k) {
    return k.ends_with(".obsidian");
}

inline bool isAnchorItem(const std::string& k) {
    return k.ends_with(".respawn_anchor");
}

// Any: a full solid block item.
inline bool isFullBlock(const std::string& k) {
    if (!blockItem(k) || isAnchorItem(k) || k.ends_with(".air")) return false;
    for (const char* s : {".glowstone", ".obsidian", ".crying_obsidian", ".cobblestone", ".stone",
                          ".deepslate", ".cobbled_deepslate", ".netherrack", ".end_stone", "_planks",
                          ".dirt", ".sand"}) {
        if (k.ends_with(s)) return true;
    }
    for (const char* s : {"_slab", "_stairs", "_fence", "_fence_gate", "_wall", "_door",
                          "_trapdoor", "_button", "_pressure_plate", "_sign", "_torch", "_carpet",
                          "_pane", "_bed", "_banner", "_sapling", "_flower", "_pot", "_rail",
                          "_ladder", "_vine", "_bars", "_chain", "_lantern", "_lever", "_candle",
                          "_head", "_skull", "_glass", ".scaffolding", ".cobweb", ".snow",
                          ".water", ".lava", "_leaves", ".bell", ".anvil", ".chest", ".hopper",
                          ".cauldron", ".composter", ".grindstone", ".lectern", ".enchanting_table",
                          ".end_rod", ".lightning_rod", "_coral", "_fan", ".bamboo", ".cactus",
                          ".sugar_cane", ".kelp", ".seagrass", "_grass", ".fern", "_bush",
                          ".lily_pad", ".dripstone", ".pointed_dripstone", ".conduit",
                          ".daylight_detector", ".repeater", ".comparator", ".redstone_wire",
                          ".tripwire_hook", ".string", ".flower_pot"}) {
        if (k.ends_with(s)) return false;
    }
    return true;
}

inline std::string at(Block b) {
    return std::to_string(b.x) + " " + std::to_string(b.y) + " " + std::to_string(b.z);
}

} // namespace vanta::modules::crystal_geom
