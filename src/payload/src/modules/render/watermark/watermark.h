// watermark.h — floating "Wrath" watermark with a W mark, wordmark
// and version, drawn over the frosted-glass shader (its own al_glass
// instance — the arraylist keeps its own).

#pragma once

#include <functional>

namespace vanta::modules::watermark {

extern bool g_enabled;

struct settings {
    float scale         = 1.0f;   // 0.5..2.5 — global size multiplier
    float rounding      = 1.0f;   // 0..2, 2 = fully-rounded pill (matches arraylist)
    bool  gradient_text = true;   // true = animated 2-stop, false = flat accent
    bool  acrylic       = true;
    /* Accent colour drives the "Wrath" text (and its gradient partner when
       gradient_text is on). The version tag stays muted white. */
    float accent_r = 155.f/255.f, accent_g = 75.f/255.f, accent_b = 220.f/255.f, accent_a = 1.f;
    /* Glass background config is INTENTIONALLY not exposed — the watermark
       reuses the exact same shader / blur pipeline the arraylist uses
       (blur_passes is pulled live from arraylist::g_settings so tuning it
       there applies here too). Keeping the two visually consistent was an
       explicit user request. */
};
extern settings g_settings;

extern std::function<void(settings&)> g_settings_provider;
struct writers {
    std::function<void(const char*, bool)>  bool_;
    std::function<void(const char*, float)> float_;
    std::function<void(const char*, int)>   int_;
    std::function<void(const char*, float, float, float, float)> color;
};
extern writers g_writers;

/* Split like arraylist: shader glass runs BEFORE NewFrame with raw GL,
   text + logo runs INSIDE the frame on the foreground draw list. */
void render_background(int display_w, int display_h, unsigned int bound_fb);
void render_text(int display_w, int display_h);

// HUD editor drag hooks.
void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);

} // namespace vanta::modules::watermark
