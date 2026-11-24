// font_pool.h — dedicated ImFontAtlas for the ArrayList.
//
// The vanta gui atlas is DPI-adaptive and rebakes every scale change, but it
// only ever bakes a narrow window of small sizes (9.5..16.5 nominal, scaled
// by the current panel_scale). Anything bigger than the top of that window
// gets rendered by ImGui upsampling small glyphs, and the result reads as
// low-resolution — the "pixelated fonts" the user reports.
//
// This pool keeps a SEPARATE atlas backing a spread of larger sizes at high
// oversampling, uploads its own GL texture, and is picked over the vanta
// atlas by the arraylist. The vanta atlas keeps doing its own thing for the
// panel itself, unaffected.

#pragma once

#include <string>

#include "imgui.h"

namespace vanta::modules::arraylist {

class font_pool {
public:
    /* @the face is a constructor argument now.

       The pool was hardwired to the gui's bold face, which is right for the
       arraylist that owns it and wrong for anything else that wants a ladder.
       The default still bakes proximabd, so every existing user is unchanged. */
    font_pool();
    explicit font_pool(std::string ttf_asset_path);
    ~font_pool() { destroy(); }
    font_pool(const font_pool&)            = delete;
    font_pool& operator=(const font_pool&) = delete;

    // Builds the atlas + uploads the GL texture on first call. Returns
    // true once the pool is ready. Safe to call every frame — cheap when
    // already built. Rebuilds if a previous ensure() half-completed.
    bool ensure();

    // Which cut to sample. `roman` is Proxima Bold, baked across the whole
    // size ladder — that is what the HUD uses. `italic` is a REAL italic
    // borrowed from the system, baked only at wordmark sizes and only over
    // the letters the wordmark needs (see k_italic_ranges).
    //
    // It exists because a synthesised italic is a roman that leans, and a
    // lean is not what makes an italic: the letterforms differ. The 'a' of a
    // true italic is single-storey, and no shear will produce that.
    enum class face { roman = 0, italic };

    // Returns the ImFont* whose FontSize is the smallest still >= desired,
    // or (fallback) the largest bake in the pool if the caller wants more
    // than we baked. AddText renders at the desired size regardless — the
    // pick just decides which atlas glyph the shader samples.
    //
    // Asking for `italic` on a machine with no italic installed falls back to
    // the roman cut, and the caller is expected to shear it themselves.
    ImFont* get(float desired_pixel_size, face which = face::roman);

    // True once a real italic was found and baked.
    bool has_italic() const { return italic_first_ >= 0; }

    // The pool's GL atlas texture — must be pushed on the target draw list
    // before any AddText for this pool's fonts (ImDrawList doesn't rebind
    // atlas textures on its own).
    ImTextureID texture_id() const;

    void destroy();

private:
    ImFontAtlas  atlas_{};
    unsigned int gl_texture_ = 0;
    bool         built_      = false;

    // Index into atlas_.Fonts where the italic bakes begin; -1 when no italic
    // face was found. Everything before it is the roman ladder.
    int          italic_first_ = -1;

    /* Asset-root-relative. Only the default face has an italic cut worth
       looking for, so the search below is gated on it. */
    std::string  ttf_asset_path_;
};

// Singleton instance the renderer talks to.
font_pool& shared_pool();

} // namespace vanta::modules::arraylist
