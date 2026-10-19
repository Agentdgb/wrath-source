#include "modules/visuals/bloom/BloomCompat.h"
#include <iostream>
using namespace vanta::modules::bloom;
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
        level.height = [](int, int) { return 64; };
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
        for (int frame = 0; frame < 2400; ++frame) {
            RegionPassCache::clear();
            RegionPassCache::now = frame / 120.;
            world::a();
            require(RegionPassCache::nLA <= 8192 && RegionPassCache::nQT <= 4096 && RegionPassCache::nSA <= 4096 &&
                        RegionPassCache::nST <= 1024,
                    "Arena capacity exceeded");
            geometry += RegionPassCache::nLA + RegionPassCache::nQT + RegionPassCache::nSA + RegionPassCache::nST;
        }
        require(geometry > 1000, "Particle simulation emitted no geometry");
        RegionPassCache::bh = false;
        for (int i = 0; i < 120; ++i) {
            RegionPassCache::clear();
            world::a();
        }
        require(!world::visible(), "Particle fade never stops");
        world::d();
        require(!world::visible(), "World reset retained particles");
        std::cout << "Bloom WorldParticles passed: Java RNG, array ownership/bounds, all five types, 2400 frames ("
                  << geometry << " primitives), disable and reset.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
