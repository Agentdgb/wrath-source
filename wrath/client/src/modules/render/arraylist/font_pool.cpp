#include "font_pool.h"

#include <fstream>

#include "util/Log.h"
#include "vgui/core/paths.hh"

#include <windows.h>
#include <gl/GL.h>

namespace vanta::modules::arraylist {

namespace {

/* Every size the arraylist / watermark can render at, in atlas pixels.

   The earlier set jumped straight from 32 to 40; on a scale=0.5 render
   the requested pixel size drops below 8 fb-px, which meant the 32 bake
   was doing a 4x downscale and reading soft. The small end matters most
   for the watermark's `v2.0` and `vldwd` sub-lines, so the low range is
   packed tightly (12, 16, 20, 24, 28) to keep the downscale factor
   under ~1.5x at every reasonable scale. The high end stays coarse — a
   3× downscale is fine once glyphs are past ~40 px. */
constexpr float k_sizes[] = {
    12.f, 16.f, 20.f, 24.f, 28.f,
    32.f, 40.f, 48.f, 56.f, 64.f, 80.f, 96.f
};

/* Include extended Latin used by lyrics (for example T/t with a caron
   or comma below), as well as typographic punctuation. */
constexpr ImWchar k_ranges[] = {
    0x0020, 0x024F,
    0x1E00, 0x1EFF,
    0x2010, 0x2027,
    0x2032, 0x203A,
    0
};

/* @the italic cut, best first.

   The test that matters is the lowercase 'a'. A TRUE italic redraws it as a
   single-storey, cursive form; a sloped roman just leans the two-storey one
   over and hopes. Segoe UI and Arial are sloped romans — Segoe was tried first
   here and the wordmark came out looking like tilted body text, which is
   exactly the complaint a sloped roman earns.

   Corbel, Calibri and Candara all ship genuine italics. Corbel's is the
   cleanest and the closest in feel to the Proxima the rest of the client is
   set in, so it leads. Segoe stays on the list, last, purely as something
   rather than nothing on a machine with none of the others. */
constexpr const char* k_italic_candidates[] = {
    "C:/Windows/Fonts/corbelz.ttf",   // Corbel Bold Italic     — true italic
    "C:/Windows/Fonts/calibriz.ttf",  // Calibri Bold Italic    — true italic
    "C:/Windows/Fonts/candaraz.ttf",  // Candara Bold Italic    — true italic
    "C:/Windows/Fonts/segoeuiz.ttf",  // Segoe UI Bold Italic   — sloped roman
};

/* Wordmark sizes, and only those. The intro asks for roughly 0.72 x the mark's
   height, which on a 1080p frame is about 160 fb-px and on 1440p over 200 — the
   roman ladder tops out at 96, so the wordmark was being upscaled nearly 2x and
   came out soft. These go past what is asked for so it never is. */
constexpr float k_italic_sizes[] = { 96.f, 176.f };

/* Lowercase only. The italic has exactly one consumer — the inject sequence's
   wordmark — and baking Latin-1 at 176 px would cost more atlas than the whole
   rest of the client put together. If that wordmark ever grows a capital or a
   digit, widen this. */
constexpr ImWchar k_italic_ranges[] = { 0x0061, 0x007A, 0 };

} // namespace

font_pool& shared_pool()
{
    static font_pool p{};
    return p;
}

font_pool::font_pool()
    : ttf_asset_path_("fonts/proximabd.ttf") {}

font_pool::font_pool(std::string ttf_asset_path)
    : ttf_asset_path_(std::move(ttf_asset_path)) {}

bool font_pool::ensure()
{
    if (built_ && gl_texture_ != 0)
        return true;

    // Wipe any half-built state from a previous ensure() attempt.
    destroy();

    const auto embedded = ::core::find_embedded_asset(ttf_asset_path_);
    const std::string path = ::core::asset_path(ttf_asset_path_.c_str());
    if (!embedded.valid() && path.empty()) {
        VLOG_WARN("arraylist", "font_pool: no asset root — can't resolve {}", ttf_asset_path_);
        return false;
    }

    ImFontConfig cfg{};
    /* Oversample the rasterisation itself for extra subpixel detail — the
       glyph is drawn 4x wider than one pixel and then averaged down inside
       stb_truetype. Combined with baking at 32-96 fb-px, this gives the
       ImGui backend so much source detail that even a 1:2 downscale to
       16 fb-px keeps every stem clean. Vape's font renderer does the
       equivalent thing via FreeType's LCD lookup. */
    cfg.OversampleH        = 4;
    cfg.OversampleV        = 2;
    cfg.PixelSnapH         = false;
    cfg.RasterizerMultiply = 1.05f;  // slightly thicker glyphs, easier to read on glass

    for (const float size : k_sizes) {
        ImFontConfig this_cfg = cfg;
        ImFont* added = nullptr;
        if (embedded.valid()) {
            this_cfg.FontDataOwnedByAtlas = false;
            added = atlas_.AddFontFromMemoryTTF(
                const_cast<std::uint8_t*>(embedded.data),
                static_cast<int>(embedded.size),
                size, &this_cfg, k_ranges);
        } else {
            added = atlas_.AddFontFromFileTTF(path.c_str(), size, &this_cfg, k_ranges);
        }
        if (added == nullptr) {
            VLOG_WARN("arraylist", "font_pool: AddFont failed at {}px", size);
        }
    }

    /* --- the italic cut, if this machine has one ------------------------- */
    italic_first_ = -1;

    /* @only for the face that has one.

       The candidates below are italic cuts of the gui's own bold face. A pool
       built for some other TTF would bake a full second ladder of them and then
       never be able to reach it, because nothing asks a Spotify pool for
       italics. */
    const bool wants_italic = ttf_asset_path_ == "fonts/proximabd.ttf";

    for (const char* candidate : k_italic_candidates) {
        if (!wants_italic)
            break;

        if (!std::ifstream(candidate).good())
            continue;

        const int before = atlas_.Fonts.Size;

        for (const float size : k_italic_sizes) {
            ImFontConfig italic_cfg{};
            /* No oversampling here, unlike the roman ladder. At 80 px and up a
               glyph already has more than enough samples per stem, and the
               oversample multiplies the atlas footprint by sixteen. */
            italic_cfg.OversampleH        = 1;
            italic_cfg.OversampleV        = 1;
            italic_cfg.PixelSnapH         = false;
            italic_cfg.RasterizerMultiply = 1.0f;

            atlas_.AddFontFromFileTTF(candidate, size, &italic_cfg, k_italic_ranges);
        }

        if (atlas_.Fonts.Size > before) {
            italic_first_ = before;
            VLOG_INFO("arraylist", "font_pool: italic cut from {}", candidate);
        }
        break;
    }

    if (!atlas_.Build()) {
        VLOG_WARN("arraylist", "font_pool: atlas Build() failed");
        atlas_.Clear();
        return false;
    }

    /* Upload the alpha8 mask as an RGBA texture. RGBA32 lets us keep the
       same shader path the rest of vanta uses (a single sampled-white glyph
       tinted by the vertex colour). */
    unsigned char* pixels = nullptr;
    int tex_w = 0, tex_h = 0;
    atlas_.GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);
    if (pixels == nullptr || tex_w < 1 || tex_h < 1) {
        VLOG_WARN("arraylist", "font_pool: atlas returned an empty pixel buffer");
        atlas_.Clear();
        return false;
    }

    glGenTextures(1, &gl_texture_);
    if (gl_texture_ == 0) {
        VLOG_WARN("arraylist", "font_pool: glGenTextures returned 0");
        atlas_.Clear();
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     0x812F /*GL_CLAMP_TO_EDGE*/);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Tell ImGui the TexID for this atlas — the render backend cross-checks
       against it, and every ImFont in the atlas exposes ContainerAtlas so
       AddText looks up TexID this way. Store the GL texture name in
       ImTextureID (a uint64 in modern ImGui). */
    atlas_.TexID = static_cast<ImTextureID>(gl_texture_);

    // ImGui recommends freeing the CPU pixel copy once uploaded.
    atlas_.ClearTexData();

    built_ = true;
    VLOG_INFO("arraylist", "font_pool: built {} bakes, atlas {}x{} px",
              static_cast<int>(sizeof(k_sizes) / sizeof(k_sizes[0])), tex_w, tex_h);
    return true;
}

ImFont* font_pool::get(float desired_pixel_size, face which)
{
    if (!built_)
        return nullptr;

    /* Which slice of the atlas to search. The roman ladder is everything before
       italic_first_; the italic bakes are everything from it on. Asking for an
       italic we never baked quietly returns the roman one, and the caller
       shears it — see the fallback in the intro. */
    const bool want_italic = (which == face::italic) && (italic_first_ >= 0);

    const int first = want_italic ? italic_first_ : 0;
    const int last  = want_italic ? atlas_.Fonts.Size
                                  : (italic_first_ >= 0 ? italic_first_ : atlas_.Fonts.Size);

    ImFont* best_ge = nullptr;   // smallest bake >= desired — minimises upscale
    ImFont* biggest = nullptr;

    for (int i = first; i < last; ++i) {
        ImFont* f = atlas_.Fonts[i];
        if (f == nullptr || f->FontSize <= 0.f)
            continue;
        if (biggest == nullptr || f->FontSize > biggest->FontSize)
            biggest = f;
        if (f->FontSize >= desired_pixel_size) {
            if (best_ge == nullptr || f->FontSize < best_ge->FontSize)
                best_ge = f;
        }
    }
    return best_ge != nullptr ? best_ge : biggest;
}

ImTextureID font_pool::texture_id() const
{
    return static_cast<ImTextureID>(gl_texture_);
}

void font_pool::destroy()
{
    if (gl_texture_ != 0) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
    atlas_.Clear();
    built_ = false;
}

} // namespace vanta::modules::arraylist
