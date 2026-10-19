#include "modules/visuals/bloom/PetalCompat.h"
#include <fstream>
#include <iostream>
using namespace vanta::modules::bloom;
namespace vanta::modules::bloom {
MinecraftClient client;
MinecraftClient *MinecraftClient::getInstance() { return &client; }
int RegionPassCache::col(int) { return 0xC080FF; }
} // namespace vanta::modules::bloom
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
        petals::GuiCanvas canvas;
        std::ifstream anchors(BLOOM_TEST_ANCHORS, std::ios::binary);
        char header[4]{};
        std::uint32_t count = 0;
        anchors.read(header, 4);
        anchors.read(reinterpret_cast<char *>(&count), 4);
        require(std::string(header, 4) == "WBA1" && count == 49 * 11, "Original branch anchor fixture invalid");
        petals::BatchArrayCache::am = Array<float>(count);
        for (unsigned i = 0; i < count; ++i) {
            anchors.read(reinterpret_cast<char *>(&petals::BatchArrayCache::am[i]), 4);
            require(std::isfinite(petals::BatchArrayCache::am[i]), "Nonfinite branch anchor");
        }
        require(bool(anchors), "Truncated branch anchors");
        std::size_t petalsDrawn = 0;
        canvas.emit = [&](const auto &p, float u0, float v0, float u1, float v1, std::uint32_t) {
            for (auto &v : p)
                require(std::isfinite(v[0]) && std::isfinite(v[1]), "Invalid petal vertex");
            require(u0 >= 0 && v0 >= 0 && u1 <= 1 && v1 <= 1, "Petal atlas coordinates outside texture");
            ++petalsDrawn;
        };
        for (int density = 1; density <= 2; ++density) {
            petals::reset();
            petals::SyncController::l = density;
            petals::b();
            for (int frame = 1; frame <= 2400; ++frame) {
                petals::c(&canvas, 1120, 700, 1, 0, 0, std::int64_t(frame) * 8333333);
                require(canvas.matrices.stack.empty(), "Unbalanced petal matrices");
            }
        }
        require(petalsDrawn > 1000, "Falling petals emitted no geometry");
        petals::reset();
        require(!petals::d(), "Petal reset failed");
        std::cout << "Bloom scenes passed: seven target styles (" << geometry << " primitives), both petal densities ("
                  << petalsDrawn << " sprites), matrix balance, atlas bounds and reset.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
