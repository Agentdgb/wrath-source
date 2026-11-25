// hitfx.h — comic hit-effects engine (POW/BAM words bursting off hit players).
//
// Faithful C++ port of Breeze's HitFX. Particles live in 3D world space with a
// decelerating burst; each frame they are projected to 2D via the captured
// render matrices (hitfx_render_context / hitfx_capture_hook) and drawn as flat
// billboards on the ImGui foreground draw list, exactly like Breeze.
//
// Hit detection: polls nearby players' hurtTime each frame (the same signal the
// server's entity-status packet sets — status 2 == hurt), so no packet-event
// path is needed. See hitfx.cpp.

#pragma once

#include <functional>

namespace dt::modules::hitfx {

extern bool g_enabled;

enum class mode : int { Comic = 0, Anime = 1 };

// Live settings, written by HitFxModule, read by the engine. Mirrors Breeze's
// HitFX settings (Rainbow, Color, Scale) plus a Mode (Comic / Anime).
struct settings {
    mode  which = mode::Comic;
    bool  rainbow = false;
    float col_r = 1.0f, col_g = 0.80f, col_b = 0.05f, col_a = 1.0f;  // Comic default gold (Breeze)
    float scale  = 1.0f;    // 0.25 .. 3.0
    float range  = 20.0f;   // only players within this many blocks trigger
    bool  glow   = true;    // soft coloured glow behind the crisp word (Breeze's shadow pass)
};
extern settings g_settings;

// Installed by HitFxModule; the engine pulls it once per frame so ClickGui
// edits (mode switch, colour, scale) take effect live.
extern std::function<void(settings&)> g_settings_provider;

// Installs the dedicated hooks (matrix capture) and marks the engine live.
// Called from HitFxModule::onInit (JNI-safe point).
void install();
void shutdown();

// Called every rendered frame from gl_hook's foreground (text) pass. Polls for
// new hits, advances + projects + draws all live particles. No-op when disabled.
void render_text(int display_w, int display_h);

} // namespace dt::modules::hitfx
