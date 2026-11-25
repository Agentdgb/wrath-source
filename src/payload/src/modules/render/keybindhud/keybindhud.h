#pragma once
namespace dt::modules::keybindhud {
inline bool enabled = false;
void render_background(int width, int height, unsigned int framebuffer);
void render_text(int width, int height);
void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);
}
