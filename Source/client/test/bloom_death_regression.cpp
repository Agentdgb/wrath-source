#include "modules/visuals/bloom/BloomCompat.h"
#include "modules/visuals/bloom/DeathEffects.h"
#include <iostream>
using namespace vanta::modules::bloom;
namespace vanta::modules::bloom {
MinecraftClient client;
MinecraftClient *MinecraftClient::getInstance() { return &client; }
int RegionPassCache::col(int value) { return value < 0 ? 0xC080FF : value; }
} // namespace vanta::modules::bloom
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        NativeWorld level;
        level.trace = [](double, double from, double, double to) {
            return to < from ? 64. : std::numeric_limits<double>::infinity();
        };
        level.visible = [](double, double, double) { return true; };
        level.sound = [](int, double, double, double) {};
        EntityView self, target;
        self.y = 64;
        target.id = 42;
        target.player = true;
        target.y = target.lastRenderY = 64;
        target.z = target.lastRenderZ = -8;
        target.dead = true;
        target.deathTime = 1;
        client.world = &level;
        client.player = &self;
        client.gameRenderer.camera.focused = &self;
        client.window = {1920, 1080};
        RegionPassCache::aa = 65.62;
        RegionPassCache::ac = 540;
        RegionPassCache::dt = 1.f / 120;
        std::size_t geometry = 0;
        for (int mode = 0; mode < 7; ++mode) {
            death::b();
            DeathSettings settings;
            settings.enabled = true;
            settings.mode = mode;
            settings.flash = true;
            death::configure(settings);
            death::g(&target, true);
            std::size_t count = 0;
            bool hidden = false;
            for (int frame = 0; frame < 480; ++frame) {
                RegionPassCache::clear();
                RegionPassCache::now = mode * 4 + frame / 120.;
                death::a();
                hidden |= death::i(target.id);
                count += RegionPassCache::nLA + RegionPassCache::nLT + RegionPassCache::nLB + RegionPassCache::nQT +
                         RegionPassCache::nQA + RegionPassCache::nSA + RegionPassCache::nST + RegionPassCache::nSB;
            }
            require(count > 0, "Death style emitted no geometry");
            require(hidden, "Corpse hide schedule missing");
            require(!death::visible(), "Death effect failed to expire");
            geometry += count;
        }
        DeathSettings settings;
        settings.enabled = true;
        settings.max = 8;
        death::configure(settings);
        for (int i = 0; i < 50; ++i)
            death::g(&target, true);
        require(death::active() == 8, "Death concurrency cap failed");
        for (int mode = 0; mode < 6; ++mode) {
            death::b();
            settings.mode = mode;
            settings.amount = 2;
            settings.scale = 4;
            death::configure(settings);
            for (int i = 0; i < 8; ++i) {
                target.id = 100 + i;
                target.player = i % 2 == 0;
                death::g(&target, true);
            }
            for (int frame = 0; frame < 360; ++frame) {
                RegionPassCache::clear();
                RegionPassCache::now = 30 + mode * 3 + frame / 120.;
                death::h();
                death::a();
                auto finite = [&](const Array<float> &data, int count, int stride, int coords) {
                    require(count >= 0 && std::size_t(count * stride) <= data.size(), "Death arena overflow");
                    for (int i = 0; i < count; ++i)
                        for (int k = 0; k < coords; ++k)
                            require(std::isfinite(data[i * stride + k]), "Invalid death geometry");
                };
                finite(RegionPassCache::LA, RegionPassCache::nLA, 11, 8);
                finite(RegionPassCache::LT, RegionPassCache::nLT, 11, 8);
                finite(RegionPassCache::LB, RegionPassCache::nLB, 11, 8);
                finite(RegionPassCache::QT, RegionPassCache::nQT, 25, 21);
                finite(RegionPassCache::QA, RegionPassCache::nQA, 25, 21);
                finite(RegionPassCache::SA, RegionPassCache::nSA, 8, 7);
                finite(RegionPassCache::ST, RegionPassCache::nST, 8, 7);
                finite(RegionPassCache::SB, RegionPassCache::nSB, 8, 7);
            }
        }
        death::b();
        require(!death::visible(), "Death reset failed");
        std::cout << "Death effects passed: six styles and random, " << geometry
                  << " primitives, expiry, hide schedule, concurrency and reset.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
