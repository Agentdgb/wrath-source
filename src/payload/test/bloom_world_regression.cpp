#include "modules/visuals/bloom/BloomCompat.h"
#include "modules/visuals/bloom/WorldParticleBudget.h"
#include <iostream>
using namespace vanta::modules::bloom;
int terrainQueries = 0;
namespace vanta::modules::bloom {
MinecraftClient client;
MinecraftClient *MinecraftClient::getInstance() { return &client; }
int RegionPassCache::col(int) { return 0xC080FF; }
namespace world {
void seedForTest(std::uint64_t);
}
} // namespace vanta::modules::bloom
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    try {
        world::TerrainCache cache;
        int calls = 0;
        auto height = [&](int, int) { ++calls; return 72; };
        cache.beginFrame(1);
        require(cache.get(-15, 9, 64, height) == 72, "Terrain lookup failed");
        require(cache.get(-15, 9, 64, height) == 72 && calls == 1, "Terrain cache missed reuse");
        for (int i = 0; i < 500; ++i) cache.get(i, i + 1, 64, height);
        require(calls == world::TerrainCache::queriesPerFrame, "Terrain queries were not bounded");
        cache.beginFrame(2);
        require(cache.get(-15, 9, 64, height) == 72 && calls == 5, "Terrain cache did not expire");
        cache.reset();
        cache.beginFrame(2.1);
        cache.get(-15, 9, 64, height);
        require(calls == 6, "World reset retained cached terrain");
        Random exact(0);
        require(exact.nextLong() == -4962768465676381896LL, "Java nextLong sequence changed");
        Random rng(123);
        auto rngAlias = rng;
        auto initialSeed = *rng.seed;
        rngAlias.nextFloat();
        require(*rng.seed != initialSeed, "Random reference alias lost");
        Random random(0);
        require(std::abs(random.nextFloat() - .73096776f) < 1e-7f, "Java Random sequence changed");
        require(random.nextInt(100) == 48, "Java bounded Random changed");
        Array<float> source(4);
        auto alias = source;
        alias[0] = 7;
        require(source[0] == 7, "Array reference alias lost");
        bool rejected = false;
        try {
            alias[4] = 1;
        } catch (const std::out_of_range &) {
            rejected = true;
        }
        require(rejected, "Native array writes outside bounds");
        EntityView self;
        self.y = 64;
        NativeWorld level;
        level.height = +[](int, int) { ++terrainQueries; return 64; };
        client.world = &level;
        client.player = &self;
        client.window = {1920, 1080};
        client.gameRenderer.camera.focused = &self;
        RegionPassCache::z = 0;
        RegionPassCache::aa = 65.62;
        RegionPassCache::ab = 0;
        RegionPassCache::fZ = -1;
        RegionPassCache::dt = 1.f / 120;
        RegionPassCache::bh = true;
        RegionPassCache::bi = RegionPassCache::bj = RegionPassCache::bk = RegionPassCache::bl = RegionPassCache::bm =
            true;
        world::seedForTest(42);
        std::size_t geometry = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 2400; ++frame) {
            terrainQueries = 0;
            RegionPassCache::clear();
            RegionPassCache::now = frame / 120.;
            world::a();
            require(terrainQueries <= world::TerrainCache::queriesPerFrame, "Engine terrain budget exceeded");
            require(RegionPassCache::nLA <= world::maxLines && RegionPassCache::nQT <= world::maxQuads &&
                        RegionPassCache::nSA <= world::maxSprites && RegionPassCache::nST <= world::maxTrailSprites,
                    "Particle geometry budget exceeded");
            geometry += RegionPassCache::nLA + RegionPassCache::nQT + RegionPassCache::nSA + RegionPassCache::nST;
        }
        require(geometry > 1000, "Particle simulation emitted no geometry");
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Exercise each family alone at full density, then zero density and teleport/reset.
        for (int family = 0; family < 5; ++family) {
            world::d();
            RegionPassCache::bi = family == 0;
            RegionPassCache::bj = family == 1;
            RegionPassCache::bk = family == 2;
            RegionPassCache::bl = family == 3;
            RegionPassCache::bm = family == 4;
            RegionPassCache::bn = 1;
            std::size_t emitted = 0;
            for (int frame = 0; frame < 1200; ++frame) {
                terrainQueries = 0;
                RegionPassCache::now += 1. / 120;
                RegionPassCache::clear();
                world::a();
                require(terrainQueries <= world::TerrainCache::queriesPerFrame, "Respawn terrain budget exceeded");
                require(RegionPassCache::nLA <= world::maxLines && RegionPassCache::nQT <= world::maxQuads &&
                            RegionPassCache::nSA <= world::maxSprites && RegionPassCache::nST <= world::maxTrailSprites,
                        "Single-family geometry budget exceeded");
                emitted += RegionPassCache::nLA + RegionPassCache::nQT + RegionPassCache::nSA + RegionPassCache::nST;
            }
            require(emitted > 0, "A particle family stopped rendering");
        }
        world::d();
        RegionPassCache::bn = 0;
        for (int frame = 0; frame < 240; ++frame) {
            RegionPassCache::now += 1. / 120;
            RegionPassCache::clear();
            world::a();
            require(RegionPassCache::nLA + RegionPassCache::nQT + RegionPassCache::nSA + RegionPassCache::nST == 0,
                    "Zero density emitted particles");
        }
        RegionPassCache::bh = false;
        for (int i = 0; i < 120; ++i) {
            RegionPassCache::clear();
            world::a();
        }
        require(!world::visible(), "Particle fade never stops");
        world::d();
        require(!world::visible(), "World reset retained particles");
        std::cout << "Bloom WorldParticles passed: Java RNG, array ownership/bounds, all five types, 2400 frames ("
                  << geometry << " primitives in " << elapsed << " ms), terrain/geometry budgets, each family, zero density, disable and reset.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
