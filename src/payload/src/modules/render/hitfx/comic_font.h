// comic_font.h — dedicated ImFontAtlas for the hit words. Two independent
// instances: the comic pool (Breeze's badaboom.ttf) and the anime pool
// (Comfortaa-Bold, rounded/kawaii). Each owns its own atlas + GL texture.

#pragma once

#include "imgui.h"

namespace vanta::modules::hitfx {

class hit_font {
public:
    hit_font() = default;
    ~hit_font() { destroy(); }
    hit_font(const hit_font&)            = delete;
    hit_font& operator=(const hit_font&) = delete;

    // rel_ttf is relative to the asset root, e.g. "fonts/badaboom.ttf".
    void set_ttf(const char* rel_ttf) { rel_ttf_ = rel_ttf; }

    bool ensure();                       // build atlas + upload GL texture (once)
    ImFont* get(float desired_px);       // closest bake >= desired, else biggest
    ImTextureID texture_id() const;
    void destroy();

private:
    const char*  rel_ttf_ = "fonts/badaboom.ttf";
    ImFontAtlas  atlas_{};
    unsigned int gl_texture_ = 0;
    bool         built_ = false;
    bool         failed_ = false;
};

hit_font& comic_pool();   // badaboom
hit_font& anime_pool();   // comfortaa

} // namespace vanta::modules::hitfx
