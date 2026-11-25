// outline.h — Outline: a coloured silhouette traced around nearby players, and
// optionally around the items lying on the ground.
//
// WHAT IT IS, AND WHAT IT IS NOT
// ------------------------------
// Not a box, not a wireframe, and not a chams. The line follows the OUTSIDE
// EDGE of the whole figure — one closed contour around the union of the head,
// body, arms and legs — with no line where an arm crosses the chest. That last
// part is the entire difficulty, and it is why this module does not simply
// stroke the six projected cuboids: stroking them draws every internal edge as
// well, which is a wireframe wearing an outline's name.
//
// HOW THE SILHOUETTE IS FOUND
// ---------------------------
// In screen space, not in world space. Computing the union of a dozen
// projected hexagons is a polygon-boolean problem; measuring it off the screen
// is three cheap shader passes:
//
//   1. MASK.     Every part of every figure is FILLED, flat white, into an
//                offscreen buffer. The union is then simply "which texels are
//                lit" — the boolean happened in the rasteriser, for free.
//
//   2. DISTANCE. A two-pass separable Euclidean distance transform turns that
//                mask into a field holding, for every texel, how many pixels
//                away the nearest lit texel is. Horizontal pass first (nearest
//                lit texel on this row), vertical pass second (combine with the
//                row offset by Pythagoras).
//
//   3. DRAW.     The contour is a threshold on that field. d in (0, thickness]
//                is the line; d beyond it, faded, is the glow. One field serves
//                both, which is why glow costs nothing extra and why the two
//                always agree on where the edge is.
//
// The field is the reason this looks clean rather than jagged. Thresholding an
// interpolated distance is how signed-distance text stays sharp, so the buffers
// run at HALF resolution without the line going soft — the crispness comes from
// the field, not from the mask's resolution. Quarter of the pixels, half the
// taps.
//
// WHERE THE GEOMETRY COMES FROM
// -----------------------------
// chams::pose and chams::geom. Both are, by their own headers, pure arithmetic
// with no JNI, no GL and no ImGui — Minecraft's biped as data plus the
// projection maths, which is a library and not another module's guts. Player
// sampling reuses chams::jni::poll_players for the same reason the ESP does not
// re-derive the ESP's matrices: a second copy of forty intermediary names is a
// second thing to get wrong on the next Minecraft version, and a wrong
// intermediary name fails silently.
//
// Dropped ITEMS were traced too, by their own sprite's alpha, and that is gone.
// The removal is worth a line because it was not a small feature: it carried an
// atlas lookup, a textured mask pass, a spin model and a per-item motion model.
// Every one of them worked in isolation and the whole never settled — a dropped
// item is small, spins, bobs, is drawn through a display transform that differs
// by item kind, and can be a stack of offset copies, so an error anywhere shows
// as a contour that visibly does not fit. Players have none of those problems.
//
// Threading: render_background runs on the wglSwapBuffers thread, which in 1.21
// is also the thread that runs the game loop, so the JNI calls it makes are
// legal. It runs BEFORE ImGui::NewFrame — there is no ImGui frame yet, so
// nothing here may call into ImGui, timing included.

#pragma once

#include <functional>

namespace dt::modules::outline {

struct settings {
    // Line width. Not pixels: a figure fifty metres away wants the same line as
    // one at arm's length, so the setting is a multiplier and the module scales
    // it to the framebuffer. 0.50 is the default in the reference design.
    float thickness = 0.50f;

    // The line's colour. Named "Enemy color" in the panel because that is what
    // it marks; there is only one, so a player and a dropped sword share it.
    float col_r = 1.f, col_g = 0.18f, col_b = 0.28f, col_a = 1.f;

    bool  glow      = true;
    // Halo reach, same units as thickness, measured outward FROM THE LINE's
    // outer edge rather than from the silhouette — so it means the same thing
    // at every Thickness instead of being swallowed by a fat line.
    float glow_size = 2.5f;

    // How bright the halo starts. Separate from glow_size on purpose: reach and
    // intensity are different questions, and the panel only ever answered the
    // first — gl::params::glow_alpha sat at a struct default that nothing in
    // the tree ever assigned, so this control did not exist.
    float glow_alpha = 0.85f;
};

extern settings g_settings;
extern bool     g_enabled;

// Installed by OutlineModule; pulled once per frame so a slider moved in the
// ClickGui takes effect on the next frame rather than on the next world.
extern std::function<void(settings&)> g_settings_provider;

void install();
void shutdown();

// One frame. Called from gl_hook's raw-GL pass, before ImGui exists, with
// DT's context current and `composite_fbo` bound. No-op when disabled.
void render_background(int display_w, int display_h, unsigned int composite_fbo);

} // namespace dt::modules::outline
