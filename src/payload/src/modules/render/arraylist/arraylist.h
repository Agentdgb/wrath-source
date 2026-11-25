// arraylist.h — public entry points for the ArrayList HUD.
//
// The heavy work lives in arraylist.cpp; this header only exposes what
// gl_hook and the module wrapper need to talk to.

#pragma once

#include <functional>

namespace dt::modules::arraylist {

// Toggled by ArrayListModule. The renderer short-circuits when false, so
// gl_hook can call render_frame() unconditionally without paying for the
// module list walk when the HUD is off.
extern bool g_enabled;

// The render is split in two so it can straddle the ImGui frame boundary:
//
//   * render_background() runs BEFORE ImGui::NewFrame. It rasterises the glass
//     shader directly onto the currently bound framebuffer and stashes the row
//     layout so the text pass can reuse it without recomputing.
//   * render_text() runs INSIDE the ImGui frame (after NewFrame, before
//     Render). It queues the module names on the foreground draw list — which
//     gl_hook.cc must skip during the panel-scale transform, so the coords
//     stay in framebuffer pixels.
void render_background(int display_w, int display_h, unsigned int bound_fb);
void render_text(int display_w, int display_h);

// Called from ArrayListModule::onDisable — frees the shader/atlas resources
// on the render thread the next chance it gets. If Java/GL is torn down
// mid-shutdown we just drop the references without touching GL.
void request_teardown();

// Live settings — populated each frame by g_settings_provider (installed by
// ArrayListModule). Keeping the module-side settings behind a callback means
// the renderer never touches the settings API directly and pulls only ONE
// live snapshot per frame, dodging any threading concerns around ClickGui
// writes that happen mid-frame on other threads.
struct settings {
    int   style            = 0;       // 0: Default, 1: Mono
    bool  background       = true;
    bool  glow             = true;
    bool  lowercase        = false;
    bool  show_settings    = true;    // append "· 90 · Legit"-style subtext per row
    float scale            = 1.0f;    // 0.5..3.0
    int   blur_passes      = 2;       // 1..5
    float rounding         = 1.0f;    // 0..2, multiplier on the base radius

    /* Text colour path — one user colour. When gradient is on, we derive a
       second harmonising stop from that same colour by shifting hue ~35°,
       so the user picks *one* accent and the gradient is guaranteed to
       "match" — green stays green-family, orange stays warm, etc. */
    bool  gradient_text    = true;
    float text_r = 70.f/255.f, text_g = 130.f/255.f, text_b = 1.f, text_a = 1.f;

    bool  cat_combat       = true;
    bool  cat_movement     = true;
    bool  cat_misc         = true;
    bool  cat_visuals      = false;
};
extern settings g_settings;

// ArrayListModule installs this in its constructor; renderers call it once at
// the top of each frame to refresh g_settings from the live setting values.
// The default is a no-op — safe to invoke even if the module never registered.
extern std::function<void(settings&)> g_settings_provider;

// Reverse channel: the HUD editor calls these to push a widget edit back into
// the real dt::Setting so it persists across sessions. The module installs
// them at construction (setting_writer.bool_ etc.). All names are the exact
// settings names ArrayListModule registered (case sensitive).
struct setting_writers {
    std::function<void(const char* name, bool  v)>  bool_;
    std::function<void(const char* name, float v)>  float_;
    std::function<void(const char* name, int   v)>  int_;
    std::function<void(const char* name, float r, float g, float b, float a)> color;
};
extern setting_writers g_setting_writers;

// ── HUD editor integration ──────────────────────────────────────────────────
//
// The HUD editor needs three hooks into the arraylist:
//   * query_bbox   — the bounding box on this frame (framebuffer pixels,
//                    top-left origin) so the editor can hover / drag it.
//   * set_top_left — pushed by drag: stores the anchor override the layout
//                    reads next frame.
//   * draw_editor_settings — renders a compact settings summary inside the
//                    editor's slide-in panel. Returns the height consumed.

void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);
float draw_editor_settings(float x, float y, float w);

} // namespace dt::modules::arraylist
