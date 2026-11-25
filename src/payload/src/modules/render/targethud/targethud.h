// targethud.h — the panel that shows who you are fighting.
//
// Split like the arraylist and the watermark, and for the same reason: the
// frosted background is a raw-GL shader pass that has to run against the game's
// framebuffer BEFORE any ImGui state exists this frame, while the head, the name
// and the bar are ImGui draw commands that have to run inside the frame. The two
// halves share one layout, computed once in the background pass, so they cannot
// disagree about where the panel is — which matters far more here than for the
// other two, because in Follow mode the panel moves every frame.

#pragma once

#include <functional>

namespace dt::modules::targethud {

extern bool g_enabled;

enum class visibility : int
{
    on_target = 0,   /* only while there is someone to show */
    always,          /* keep the last target on screen */
};

struct settings
{
    float scale = 1.2f;        /* 0.5 .. 2.5 */
    float rounding = 1.0f;     /* 0 .. 2, multiplies the base corner radius */

    int   font = 0;            /* index into targethud::face_names() */
    int   show = 0;            /* visibility */

    float linger = 4.0f;       /* seconds a lock survives without a fresh hit */

    /* @blocks, past which the lock is dropped.

       The reference client has this and it is not a setting there: its target
       provider carries a per-target range budget and nulls the target the tick
       the distance exceeds it, ten blocks for a melee hit. Without an equivalent
       the panel stays pinned to someone half a render distance away — the same
       230x76 pixels beside a player who is now four pixels tall, which is what
       "it gets huge when I back off" actually describes. A setting rather than a
       constant because the reference's ten is a melee number and this panel is
       also used to watch someone walk away. */
    float range = 16.0f;       /* 4 .. 64 */

    /* Anchor to the target in the world. A plain boolean, like the reference
       client's: the smoothing is two fixed half-lives, and exposing them as a
       slider is what would stop this being the same configuration. */
    bool  follow = false;

    bool  show_distance = true;
};

extern settings g_settings;

/* Installed by the module so the renderer can pull live values without the
   render pass ever touching a Setting object — the same contract the watermark
   and the ESP use. */
extern std::function<void(settings&)> g_settings_provider;

/* The frosted pane. Raw GL, before ImGui::NewFrame, straight onto the bound
   framebuffer. Also the pass that polls the target and lays the panel out. */
void render_background(int display_w, int display_h, unsigned int bound_fb);

/* Head, name, health bar and readout, on ImGui's foreground draw list in
   framebuffer pixels. */
void render_text(int display_w, int display_h);

/* HUD editor drag hooks. The drag is honoured in every mode, Follow included:
   Follow is suspended while the editor is open, so what is being dragged there
   is the fixed anchor — which is also the position Follow itself falls back to
   on any frame it cannot project the target. */
void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);

} // namespace dt::modules::targethud
