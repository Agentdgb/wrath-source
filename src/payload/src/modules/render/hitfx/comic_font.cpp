#include "comic_font.h"
#include "hitfx_debug.h"

#include <string>
#include <string_view>

#include "vgui/core/paths.hh"

#include <windows.h>
#include <gl/GL.h>

namespace dt::modules::hitfx {

namespace {
// The hit words render large and are scaled per particle, so bake a spread of
// big sizes. Breeze's native is 150px; we bracket that so downscales stay mild.
constexpr float k_sizes[] = { 40.f, 56.f, 72.f, 96.f, 128.f, 160.f };
constexpr ImWchar k_ranges[] = { 0x0020, 0x00FF, 0 };   // ASCII + Latin-1 (covers "uwu", "7w7", "OwO")
}

hit_font& comic_pool()
{
    static hit_font p{};
    static bool init = [] { p.set_ttf("fonts/badaboom.ttf"); return true; }();
    (void)init;
    return p;
}

hit_font& anime_pool()
{
    // Sniglet ExtraBold — a rounded, chunky, kawaii display face; much more
    // "filled" than Comfortaa, so the cute words read solid.
    static hit_font p{};
    static bool init = [] { p.set_ttf("fonts/anime.ttf"); return true; }();
    (void)init;
    return p;
}

bool hit_font::ensure()
{
    if (built_ && gl_texture_ != 0) return true;
    if (failed_) return false;

    destroy();

    const auto embedded = ::core::find_embedded_asset(rel_ttf_);
    const std::string path = ::core::asset_path(rel_ttf_);
    if (!embedded.valid() && path.empty()) {
        dbg::log("hit_font: asset root unresolved (%s)", rel_ttf_);
        failed_ = true;
        return false;
    }

    ImFontConfig cfg{};
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = false;
    cfg.RasterizerMultiply = 1.0f;

    int baked = 0;
    for (const float size : k_sizes) {
        ImFontConfig c = cfg;
        ImFont* added = nullptr;
        if (embedded.valid()) {
            c.FontDataOwnedByAtlas = false;
            added = atlas_.AddFontFromMemoryTTF(
                const_cast<std::uint8_t*>(embedded.data),
                static_cast<int>(embedded.size),
                size, &c, k_ranges);
        } else {
            added = atlas_.AddFontFromFileTTF(path.c_str(), size, &c, k_ranges);
        }
        if (added != nullptr)
            ++baked;
    }
    if (baked == 0) {
        dbg::log("hit_font: AddFontFromFileTTF failed for ALL sizes (path=%s)", path.c_str());
        atlas_.Clear(); failed_ = true; return false;
    }
    if (!atlas_.Build()) {
        dbg::log("hit_font: atlas Build() failed (%s)", rel_ttf_);
        atlas_.Clear(); failed_ = true; return false;
    }

    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    atlas_.GetTexDataAsRGBA32(&pixels, &tw, &th);
    if (!pixels || tw < 1 || th < 1) {
        dbg::log("hit_font: empty atlas pixels (%s)", rel_ttf_);
        atlas_.Clear(); failed_ = true; return false;
    }

    glGenTextures(1, &gl_texture_);
    if (gl_texture_ == 0) { atlas_.Clear(); failed_ = true; return false; }
    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    atlas_.TexID = static_cast<ImTextureID>(gl_texture_);
    atlas_.ClearTexData();
    built_ = true;
    dbg::log("hit_font: built %d bakes of %s, atlas %dx%d", baked, rel_ttf_, tw, th);
    return true;
}

ImFont* hit_font::get(float desired_px)
{
    if (!built_) return nullptr;
    ImFont* best_ge = nullptr;
    ImFont* biggest = nullptr;
    for (int i = 0; i < atlas_.Fonts.Size; ++i) {
        ImFont* f = atlas_.Fonts[i];
        if (!f || f->FontSize <= 0.f) continue;
        if (!biggest || f->FontSize > biggest->FontSize) biggest = f;
        if (f->FontSize >= desired_px && (!best_ge || f->FontSize < best_ge->FontSize)) best_ge = f;
    }
    return best_ge ? best_ge : biggest;
}

ImTextureID hit_font::texture_id() const { return static_cast<ImTextureID>(gl_texture_); }

void hit_font::destroy()
{
    if (gl_texture_ != 0) { glDeleteTextures(1, &gl_texture_); gl_texture_ = 0; }
    atlas_.Clear();
    built_ = false;
}

} // namespace dt::modules::hitfx
