// chams.h — Chams: the player model drawn FILLED and SOLID through walls.
//
// What a chams is, precisely
// --------------------------
// Not an outline. Not a glow. Not a wireframe. Not a bounding box. A chams
// replaces the surface of the player's body with a flat fill and draws it with
// the depth test defeated, so the whole silhouette reads through terrain.
//
// Two modes are exposed, and they are NOT the same feature wearing two colours:
//
//   Mode::Colour  — the body filled with one chosen flat colour.
//   Mode::Normal  — the body drawn with its REAL appearance (skin texture),
//                   just visible through walls.
//
// Colour is fully served by this module. Normal is NOT, and the header says so
// rather than pretending: see the note on Mode::Normal below.
//
// Why this module owns its geometry
// ---------------------------------
// Minecraft's player model is literally six cuboids — head 8x8x8, body 8x12x4,
// each arm and leg 4x12x4 model units — plus six slightly-inflated overlay
// cuboids (hat, jacket, sleeves, pants). Filling those six boxes opaque is not
// an approximation of the player silhouette; it IS the player silhouette. So
// the module reconstructs MC's animated biped pose, projects the eight corners
// of each cuboid, and fills the resulting quads.
//
// The PlayerESP module happens to compute similar geometry for its wireframe
// mode. This module deliberately does NOT reach into it: a render module that
// reads another render module's internals couples their lifetimes, their
// enable/disable state and their per-frame ordering, and neither can then be
// changed alone. Chams carries its own JNI cache, its own pose reconstruction
// and its own projection. The only thing shared is the hitfx matrix-capture
// service, which is already a shared service in this codebase (the ESP consumes
// it the same way) rather than another module's guts.
//
// Threading: install/shutdown come from the module lifecycle; render_text runs
// on the wglSwapBuffers thread inside an active ImGui frame, which in 1.21 is
// also the thread that runs the game loop — so JNI calls from there are legal.

#pragma once

#include <functional>

namespace vanta::modules::chams {

// ─────────────────────────────────────────────────────────────────────────────

// Live settings, written by ChamsModule via g_settings_provider, read by the
// engine once per frame so ClickGui edits take effect immediately (the same
// pattern hitfx and the ESP use — the bridge never calls a module's push()).
struct settings {
    // Everything here is engine-internal now; the module exposes no controls.
    float range       = 128.f;   // poll radius, pinned to max
    int   max_targets = 256;     // poll cap, pinned to max
    bool  showself    = false;
};
extern settings g_settings;

extern bool g_enabled;
extern std::function<void(settings&)> g_settings_provider;

// Resolves the JNI cache and the shared matrix-capture service. Idempotent and
// safe to call before a world exists — it retries internally until the classes
// resolve, and reports every lookup to the desktop debug log.
void install();
void shutdown();

// Called once per rendered frame from gl_hook's foreground pass, inside an
// active ImGui frame. Polls players, rebuilds each pose, projects, and fills.
// No-op when disabled.
void render_text(int display_w, int display_h);

// Called once per rendered frame from gl_hook, at the top of the swap detour,
// while the GAME's GL context is current — which the overlay pass above is NOT.
//
// This is where the native GL interception layer (chams_gl.h) is armed:
// wglGetProcAddress requires a current context, and it must be the context
// whose entry points Minecraft itself calls. Idempotent, and a single atomic
// read once the hooks are in place.
void tick_game_context();

} // namespace vanta::modules::chams
