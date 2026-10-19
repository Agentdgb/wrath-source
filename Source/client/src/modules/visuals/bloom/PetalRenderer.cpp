#include "Bloom.h"
#include "NativeTextures.h"
#include "PetalCompat.h"
#include "imgui.h"
#include "modules/render/arraylist/al_gl.h"
#include "modules/visuals/ports/CharacterData.h"
#include "vgui/framework/render/stb/stb_image.hh"
#include <cstring>
#include <gl/GL.h>
#include <windows.h>
namespace vanta::modules::bloom {
PetalSettings petalSettings;
namespace {
GLuint petalTexture = 0;
}
void shutdownGui() {
    if (petalTexture)
        glDeleteTextures(1, &petalTexture);
    petalTexture = 0;
    petals::reset();
    petals::BatchArrayCache::am = Array<float>(0);
}
void renderGui(int width, int height, float opacity) {
    if (!petalSettings.enabled || opacity <= .001f || width < 1 || height < 1) {
        petals::reset();
        return;
    }
    if (!ImGui::GetCurrentContext())
        return;
    if (!petalTexture) {
        try {
            auto raw = visualports::characterResource(2301);
            if (raw.size() < 8 || std::memcmp(raw.data(), "WBA1", 4))
                throw std::runtime_error("Invalid branch anchors");
            std::uint32_t count = 0;
            std::memcpy(&count, raw.data() + 4, 4);
            if (count > 66000 || count % 11 || raw.size() != 8 + count * 4)
                throw std::runtime_error("Invalid branch anchor size");
            petals::BatchArrayCache::am = Array<float>(count);
            for (unsigned i = 0; i < count; ++i)
                std::memcpy(&petals::BatchArrayCache::am[i], raw.data() + 8 + i * 4, 4);
            petalTexture = loadNativeTexture(2300, false);
        } catch (...) {
            return;
        }
    }
    petals::SyncController::l = petalSettings.density;
    if (!petals::d())
        petals::b();
    petals::GuiCanvas canvas;
    canvas.emit = [&](const auto &p, float u0, float v0, float u1, float v1, std::uint32_t argb) {
        auto abgr = (argb & 0xFF00FF00u) | ((argb & 255) << 16) | ((argb >> 16) & 255);
        ImGui::GetBackgroundDrawList()->AddImageQuad(static_cast<ImTextureID>(petalTexture), {p[0][0], p[0][1]},
                                                     {p[1][0], p[1][1]}, {p[2][0], p[2][1]}, {p[3][0], p[3][1]},
                                                     {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}, abgr);
    };
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    try {
        petals::c(&canvas, width, height, opacity, 0, 0, now);
    } catch (...) {
        petals::reset();
    }
}
} // namespace vanta::modules::bloom
