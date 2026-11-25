#include "modules/visuals/bloom/BloomCompat.h"
#include <fstream>
#include <iostream>
using namespace dt::modules::bloom;
namespace dt::modules::bloom {
MinecraftClient client;
MinecraftClient *MinecraftClient::getInstance() { return &client; }
int RegionPassCache::col(int) { return 0xC080FF; }
} // namespace dt::modules::bloom
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    try {
        EntityView self, target;
        self.y = 64;
        target.x = 0;
        target.y = 64;
        target.z = -8;
        target.lastRenderX = target.x;
        target.lastRenderY = target.y;
        target.lastRenderZ = target.z;
        NativeWorld level;
        client.world = &level;
        client.player = &self;
        client.gameRenderer.camera.focused = &self;
        client.window = {1920, 1080};
        RegionPassCache::z = 0;
        RegionPassCache::aa = 65.62;
        RegionPassCache::ab = 0;
        RegionPassCache::fZ = -1;
        RegionPassCache::dt = 1.f / 120;
        RegionPassCache::ah = true;
        RegionPassCache::aq = &target;
        RegionPassCache::am = 1;
        RegionPassCache::an = 1;
        RegionPassCache::ao = .9f;
        RegionPassCache::ap = .9f;
        std::size_t geometry = 0;
        for (int mode = 0; mode < 7; ++mode) {
            halo::p();
            RegionPassCache::aj = mode;
            std::size_t emitted = 0;
            for (int frame = 0; frame < 240; ++frame) {
                RegionPassCache::clear();
                RegionPassCache::now = (mode * 240 + frame) / 120.;
                halo::a();
                emitted += RegionPassCache::nLA + RegionPassCache::nQT + RegionPassCache::nQA + RegionPassCache::nSA;
            }
            require(emitted > 0, "Target style emitted no geometry");
            geometry += emitted;
        }
        RegionPassCache::ah = false;
        for (int frame = 0; frame < 120; ++frame) {
            RegionPassCache::clear();
            halo::a();
        }
        require(!halo::visible(), "Target fade did not finish");
        halo::p();
        std::cout << "Seven target styles passed (" << geometry << " primitives), fade and reset.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
