// radar.h — a top-down dial of the players around you.
//
// Split like the arraylist, the watermark and the Target HUD, and for the same
// reason: the frosted background is a raw-GL shader pass that has to run against
// the game's framebuffer BEFORE any ImGui state exists this frame, while the
// dial and its blips are ImGui draw commands that have to run inside the frame.
// The two halves share one layout, computed once in the background pass, so they
// cannot disagree about where the panel is.
//
// The bearing/range/fade maths is Breeze's Radar; radar_source.h says exactly
// what was taken and what was deliberately left behind.

#pragma once

#include <functional>

namespace dt::modules::radar {

extern bool g_enabled;

enum class marker : int
{
    arrow = 0,   /* a triangle pointing the way they are, Breeze's ARROW */
    dot,         /* a plain round blip */
};

struct settings
{
    float scale = 1.0f;        /* 0.5 .. 2.5 */
    float rounding = 1.0f;     /* 0 .. 2, multiplies the base corner radius */

    /* Blocks. Breeze's default and limits: 60, clamped to 10..100. */
    float range = 60.f;

    int   mode = 0;            /* marker */

    /* Breeze's own default pointer colour, kept rather than swapped for the
       client accent: the radar is read against the world, not against the rest
       of the client, and a colour that stands out on grass and stone is worth
       more here than one that matches the panel it sits in. */
    float blip_r = 230.f / 255.f;
    float blip_g = 34.f / 255.f;
    float blip_b = 100.f / 255.f;
    float blip_a = 1.f;

    /* Blips above or below you are tinted toward white / black respectively, so
       a flat dial can still say "he is on the roof". */
    bool  show_height = true;
};

extern settings g_settings;

/* Installed by the module so the renderer can pull live values without the
   render pass ever touching a Setting object — the same contract every other
   HUD module in this client uses. */
extern std::function<void(settings&)> g_settings_provider;

/* The frosted pane. Raw GL, before ImGui::NewFrame, straight onto the bound
   framebuffer. Also the pass that polls the world and lays the panel out. */
void render_background(int display_w, int display_h, unsigned int bound_fb);

/* The dial, the ticks and the blips, on ImGui's foreground draw list in
   framebuffer pixels. */
void render_text(int display_w, int display_h);

/* HUD editor drag hooks. */
void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);

/* Forgets the polled world. Called when the module is disabled so re-enabling it
   does not flash the last server's players onto the dial. */
void reset();

} // namespace dt::modules::radar
