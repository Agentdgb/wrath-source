#include "hud_font.h"

#include "util/Log.h"

#include "fonts/hud_font_data.inl"

#include <windows.h>
#include <gl/GL.h>

namespace vanta::modules::targethud {

namespace {

/* @the size ladder.

   Baking at the size the text will be drawn at is what keeps the glyphs sharp —
   the gui atlas only covers 9.5..16px, and asking it for a 30px name is what
   produced the "pixelated fonts" the arraylist pool was built to fix.

   The rungs are close together on purpose. get() picks the smallest bake that is
   still at least the requested size, so the glyphs are always DOWNscaled, never
   up — and the gap between rungs is how far they get downscaled. Five widely
   spaced rungs meant a 16px name sampled a 22px bake and came out soft; seven
   tighter ones keep every size the HUD actually asks for within a few pixels of
   a real bake. Only one face is ever resident, so the atlas stays modest.

   @13 and 26 are there for one face.

   ProggyClean is a bitmap authored at exactly 13px, and ImGui bakes it with no
   oversampling on purpose. Rasterised at 12 or 22 it lands off its own pixel
   grid and comes back with soft grey edges — which is precisely what a pixel
   face is chosen to avoid. A rung on the grid, and its double, gives get() an
   exact hit for the sizes the HUD actually asks for.

   @the ladder does not cover everything, and get() says so.

   Above the top rung the glyphs are magnified rather than reduced. That needs
   Scale past about 2.3 on a 1440p screen, which the slider does allow — the top
   rung is set high enough that reaching it takes deliberate effort. */
constexpr float k_sizes[] = { 12.f, 13.f, 15.f, 18.f, 22.f, 26.f, 32.f, 40.f, 52.f };

/* Latin-1. A username is ASCII by Mojang's own rules, and the health readout is
   digits and a slash — the extra range costs almost nothing and covers a server
   that puts an accented character in a display name. */
constexpr ImWchar k_ranges[] = { 0x0020, 0x00FF, 0 };

struct face_def
{
    const char* label;

    /* The bytes, compiled in. Null means ImGui's own ProggyClean, which it
       already carries compressed inside itself. */
    const unsigned char* data;
    std::size_t          size;

    /* A bitmap face wants no oversampling and a NEAREST filter; a smooth one
       wants the opposite. Was `embedded` until every face became embedded, at
       which point the flag was naming the wrong fact. */
    bool pixel;
};

/* @four faces, and every one of them lives in the DLL.

   The list used to be fourteen: three asset files, ten fonts read off
   C:/Windows/Fonts, and ProggyClean. The Windows ten were the problem. They are
   whatever that machine happens to have -- a different Windows edition, an N
   build, a stripped image, and the entry is a name that picks nothing. That is
   the exact failure the header warns about: you choose a font, the HUD keeps
   the old one, and the setting reads as broken.

   So the file dependency is gone entirely. Nothing here can fail to resolve,
   because there is nothing to resolve. */
constexpr face_def k_faces[] = {
    { "Proxima Bold", k_font_proxima_bold, k_font_proxima_bold_size, false },
    { "Proxima",      k_font_proxima,      k_font_proxima_size,      false },
    { "Comfortaa",    k_font_comfortaa,    k_font_comfortaa_size,    false },

    /* @named Minecraft, drawn with ProggyClean.

       The label is what was asked for and the face is the closest thing this
       repo can honestly ship: ImGui's built-in bitmap font, which reads as
       pixel art at HUD sizes. It is NOT Mojang's typeface -- that one is not
       redistributable. Dropping an OFL lookalike into assets/fonts and pointing
       this entry at it is a two-line change, and until someone does, this is a
       pixel font under a name that promises a specific one. */
    { "Minecraft",    nullptr,             0,                        true  },
};

static_assert(sizeof(k_faces) / sizeof(k_faces[0]) == face_count,
    "k_faces and face_id disagree — the dropdown would name one font and draw another");

const char* g_names[face_count] = {};

ImFontAtlas  g_atlas{};
unsigned int g_texture = 0;
bool         g_built = false;

face_id g_wanted = face_id::proxima_bold;
face_id g_baked = face_id::count;   /* nothing baked yet */

/* A face that fails to bake is remembered so a bad blob is tried once and not
   on every frame of every rebuild attempt. */
bool g_failed[face_count] = {};

void release_texture()
{
    if (g_texture != 0)
    {
        glDeleteTextures(1, &g_texture);
        g_texture = 0;
    }

    g_atlas.Clear();
    g_built = false;
}

bool build(face_id which)
{
    const int index = static_cast<int>(which);

    if (index < 0 || index >= static_cast<int>(face_count) || g_failed[index])
        return false;

    const face_def& def = k_faces[index];

    release_texture();

    ImFontConfig cfg{};
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;

    int baked = 0;

    for (const float size : k_sizes)
    {
        if (def.data == nullptr)
        {
            /* ProggyClean, out of ImGui's own compressed copy and into OUR atlas
               rather than the default one. Putting it here means every face this
               pool offers shares a single texture, so the draw code pushes one
               id and never has to know which font it is drawing with.

               No oversampling: it is a bitmap face authored at 13px, and
               filtering it is the one thing that would stop it looking like a
               pixel font. */
            ImFontConfig c{};
            c.OversampleH = 1;
            c.OversampleV = 1;
            c.PixelSnapH = true;
            c.SizePixels = size;

            /* Stated rather than inherited. AddFontDefault falls back to
               GetGlyphRangesDefault, which happens to be the same 0x20..0xFF
               this pool asks for -- so this is a no-op today and stops being one
               the moment k_ranges widens. */
            c.GlyphRanges = k_ranges;

            if (g_atlas.AddFontDefault(&c) != nullptr)
                ++baked;

            continue;
        }

        ImFontConfig c = cfg;

        /* @the atlas must NOT take the buffer.

           AddFontFromMemoryTTF assumes the data is heap memory it now owns and
           frees it on Clear() -- which for a constexpr array in .rdata is a
           free() of a read-only static, i.e. a crash on the first font switch.
           Clearing the flag makes ImGui copy the bytes instead. */
        c.FontDataOwnedByAtlas = false;

        if (g_atlas.AddFontFromMemoryTTF(
                const_cast<unsigned char*>(def.data), static_cast<int>(def.size),
                size, &c, k_ranges) != nullptr)
            ++baked;
    }

    if (baked == 0 || !g_atlas.Build())
    {
        VLOG_WARN("targethud", "could not bake '{}'", def.label);
        g_atlas.Clear();
        g_failed[index] = true;
        return false;
    }

    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    g_atlas.GetTexDataAsRGBA32(&pixels, &tw, &th);

    if (pixels == nullptr || tw < 1 || th < 1)
    {
        g_atlas.Clear();
        g_failed[index] = true;
        return false;
    }

    glGenTextures(1, &g_texture);

    if (g_texture == 0)
    {
        g_atlas.Clear();
        g_failed[index] = true;
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, g_texture);

    /* NEAREST for the pixel face, LINEAR for the rest. A bitmap font put
       through a bilinear filter is mush, and a smooth face without one has
       jagged edges — the right answer differs per face, and it is the whole
       point of offering a pixel face at all. */
    const GLint filter = def.pixel ? GL_NEAREST : GL_LINEAR;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_atlas.TexID = static_cast<ImTextureID>(g_texture);
    g_atlas.ClearTexData();

    /* @and the source blobs, which are not free.

       AddFontFromMemoryTTF with FontDataOwnedByAtlas cleared makes ImGui copy
       the buffer — once per rung. Nine rungs of Comfortaa is over a megabyte of
       heap held for nothing after the atlas is baked and uploaded. The glyphs
       live in the texture now; the TTFs have done their job. */
    g_atlas.ClearInputData();

    g_built = true;
    g_baked = which;

    VLOG_INFO("targethud", "baked '{}' ({} sizes, atlas {}x{})", def.label, baked, tw, th);
    return true;
}

} // namespace

const char* const* face_names()
{
    if (g_names[0] == nullptr)
    {
        for (std::size_t index = 0; index < face_count; ++index)
            g_names[index] = k_faces[index].label;
    }

    return g_names;
}

void select(face_id which)
{
    if (which < face_id::proxima_bold || which >= face_id::count)
        which = face_id::proxima_bold;

    g_wanted = which;
}

bool ensure()
{
    if (g_built && g_baked == g_wanted && g_texture != 0)
        return true;

    if (build(g_wanted))
        return true;

    /* @a face that will not load must not take the HUD down with it.

       Falling back to the default rather than returning false leaves the HUD
       drawn in Proxima Bold with the dropdown still showing the name the user
       picked — which is wrong, but it is visibly wrong in a way they can undo,
       where an empty HUD looks like the module is broken. The failure is
       logged once per face by build(). */
    if (g_wanted != face_id::proxima_bold)
    {
        g_wanted = face_id::proxima_bold;

        /* Back through the fast path, not straight into build(). Without this
           the fallback rebuilds the atlas on EVERY frame for as long as the
           unavailable face stays selected — a full bake plus a texture upload,
           sixty times a second, because build() itself has no "already built"
           guard; that lives at the top of this function. */
        if (g_built && g_baked == g_wanted && g_texture != 0)
            return true;

        return build(g_wanted);
    }

    return false;
}

ImFont* get(float desired_pixel_size)
{
    if (!g_built)
        return nullptr;

    ImFont* best = nullptr;
    ImFont* biggest = nullptr;

    for (int index = 0; index < g_atlas.Fonts.Size; ++index)
    {
        ImFont* font = g_atlas.Fonts[index];

        if (font == nullptr || font->FontSize <= 0.f)
            continue;

        if (biggest == nullptr || font->FontSize > biggest->FontSize)
            biggest = font;

        if (font->FontSize >= desired_pixel_size
            && (best == nullptr || font->FontSize < best->FontSize))
            best = font;
    }

    return best != nullptr ? best : biggest;
}

ImTextureID texture_id()
{
    return static_cast<ImTextureID>(g_texture);
}

void destroy()
{
    release_texture();
    g_baked = face_id::count;
}

} // namespace vanta::modules::targethud
