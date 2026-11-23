// esp.h — Player ESP (2D box + health bar, 3D box, and pack-faithful equipment
// icons) drawn on the ImGui foreground draw list.
//
// Reuses the hitfx matrix-capture service (hitfx_capture_hook) + render context
// (hitfx_render_context) for the world->screen projection, so no render method
// is hooked. Boxes are built from the entity's INTERPOLATED render position
// (lastRenderX/Y/Z lerped to getX/Y/Z by the frame's tickProgress), exactly the
// value vanilla's WorldRenderer draws at — so the box locks to the player with
// no jitter. Corners behind the camera cull the whole box, so no edges shoot
// across the screen.

#pragma once

#include <functional>

namespace vanta::modules::esp {

extern bool g_enabled;

// Live settings, written by EspModule (via g_settings_provider), read by the
// engine each frame.
struct settings {
    int   mode      = 0;       // 0 = 2D box, 1 = 3D box, 2 = wireframe (pick one)
    bool  fill      = false;   // translucent fill inside the box/wireframe
    bool  health    = true;    // vertical health bar beside the box
    bool  equipment = true;    // armor icons stacked to the side (pack textures)
    bool  showself  = false;   // also draw the ESP on your own player
    float col_r = 1.f, col_g = 1.f, col_b = 1.f, col_a = 1.f;  // ESP colour
    float thickness = 1.0f;     // GUI-style outline width for 2D/3D only
    float range = 64.f;        // only players within this many blocks
};
extern settings g_settings;

// Installed by EspModule; the engine pulls it once per frame so ClickGui edits
// take effect live (same pattern as hitfx).
extern std::function<void(settings&)> g_settings_provider;

void install();
void shutdown();

// Called every rendered frame from gl_hook's foreground (text) pass. Drives the
// matrix capture, polls nearby players, projects + draws. No-op when disabled.
void render_text(int display_w, int display_h);

} // namespace vanta::modules::esp
