// world_overlay_view.h — fold Minecraft's own view bob and hurt tilt into a
// captured camera frame, so an overlay projected through hitfx::world_to_screen
// lands where the world was actually drawn.
//
// The capture service (hitfx_capture_hook) publishes the camera ROTATION and
// the basic projection. Neither carries GameRenderer.bobView (the walking sway)
// nor tiltViewWhenHurt (the damage roll): the game multiplies both into the
// world pass after the camera is set. Project without them and an overlay is
// exact while standing still, then swings off its target with every footstep
// and every hit taken — a few degrees of view rotation, which is tens of pixels
// at any range. That is what "the highlight is wrong" and "the ghost bugs out"
// look like from the player's side.
//
// The PlayerESP (esp.cpp build_proj) and the Outline (outline.cpp build_camera)
// already fold the same matrix in, read back from GameRenderer itself by
// outline::view::bob_matrix so it is the game's bob by construction. This is
// the shared route for every other world-anchored overlay: BlockESP, the
// Backtrack ghost, the WorldTweaks particles.
//
// Layout: projection * (bob * modelview), column-major, exactly as those two.

#pragma once

#include <jni.h>
#include <cstring>

#include "imgui.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "modules/render/outline/outline_view.h"

namespace vanta::modules::overlay_view {

// C = A * B, both column-major, index [col*4 + row].
inline void mat_mul(const float* A, const float* B, float* C)
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += A[k * 4 + row] * B[col * 4 + k];
            C[col * 4 + row] = s;
        }
}

// Pre-multiplies MC's hurt tilt + walking bob onto f.modelview. Returns false
// (frame untouched) when bobbing is off, the lookup is unavailable or anything
// failed — the caller then projects the raw frame, which is what it did before.
//
// The bob is read back from GameRenderer once per ImGui frame and shared by
// every overlay that asks in that frame: each caller captures its own frame
// snapshot, but the bob is a property of the game frame, not of the snapshot.
inline bool apply_view_bob(JNIEnv* env, hitfx::render_frame& f)
{
    static int   s_frame = -1;
    static bool  s_have  = false;
    static float s_bob[16];

    const int frame = ImGui::GetFrameCount();
    if (frame != s_frame) {
        s_frame = frame;
        s_have  = env != nullptr
               && outline::view::ensure_cache(env)
               && outline::view::bob_matrix(env, s_bob);
    }
    if (!s_have) return false;

    float mv[16];
    mat_mul(s_bob, f.modelview, mv);
    std::memcpy(f.modelview, mv, sizeof(mv));
    return true;
}

} // namespace vanta::modules::overlay_view
