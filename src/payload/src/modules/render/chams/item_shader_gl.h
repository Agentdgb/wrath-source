// item_shader_gl.h — the ItemShader feature's OWN native GL layer.
//
// The first-person held-item glow used to live inside chams_gl.cpp because it
// rides on the same draw interception. But the detection + capture + blur +
// composite pipeline is the ItemShader's, not the Chams's — so it lives here, in
// its own translation unit, and chams_gl.cpp only calls into it at three points:
//
//   * consider_fp_draw() — from the draw classifier, for each draw seen inside
//     the first-person window, to decide whether it is the held item.
//   * run_capture()      — from the draw macro, to replay the item into our FBO.
//   * on_frame_boundary()— once per frame, to build programs + blur + composite.
//
// Everything else (state, GL function pointers, FBOs, shaders, the blur/composite
// passes) is owned here. The layer touches GL through the shared dt::al::gl
// loader + raw GL 1.1, bracketed by chams' suppression so it never pollutes the
// Chams shadow — it does NOT reach into any Chams internals.

#pragma once

#include <cstdint>
#include <functional>

namespace dt::modules::itemshader {

// ── Controls (driven by the ItemShader module + the Animations detour) ────────

// Master gate: the ItemShader module is on. chams.cpp ORs this into its layer arm
// so the GL interception runs even with the Chams module off.
void set_wanted(bool on);
bool wanted();

// The effect itself (draw the glow). Kept separate from `wanted` so the module can
// arm the layer a frame before it starts drawing.
void set_effect(bool on);

// Verbose per-frame logging to <Desktop>\dt_itemshader.txt. Module Debug drives it.
void set_debug(bool on);

// Raised by the Animations first-person detour around renderFirstPersonItem — the
// LOCAL player's own viewmodel item, and only that. Left up until the frame
// boundary to also cover the 1.21.9+ deferred render-queue emit.
void set_rendering_firstperson(bool on);
bool rendering_firstperson();

// Called by the Animations detour at the ENTRY of renderFirstPersonItem.
void note_firstperson_render_call();

// ── Hooks the Chams draw layer calls into ─────────────────────────────────────

// Cheap: is the feature armed at all (wanted && effect)? chams uses it to skip the
// per-draw consideration entirely when the module is off.
bool armed();

// Called for every draw seen inside the first-person window (chams supplies the
// program + its classification, since chams owns the per-program tables). Returns
// true when THIS draw is the first-person held item and the macro should capture
// it. Also drives the HUD latch that keeps hotbar/inventory item models — which
// are 3D and fall in the same window — from being captured.
bool consider_fp_draw(unsigned int program, int count, bool is_entity,
                      bool has_normal, unsigned int tex0);

// Called by the draw macro when consider_fp_draw armed this draw: replays the
// item into the offscreen FBO (real bloom) or stamps the fallback outline.
void run_capture(const std::function<void()>& redraw);

// Called once per frame at the frame boundary, game context: build programs,
// blur + composite this frame's capture, dump diagnostics, roll per-frame state.
void on_frame_boundary();

} // namespace dt::modules::itemshader
