#pragma once

#include <jni.h>

namespace dt::modules::backtrack {

// Called in the existing foreground world-overlay pass (gl_hook.cc), after
// the ESP and before BlockESP. Draws the ghost hitbox.
void render(int width, int height);

// Backtrack ↔ triggerbot seam.
//
// When the local crosshair ray lands on `entity`'s ghost hitbox (the sample
// from `Delay` ms ago) and NOT on its real body, this module has already
// pointed MinecraftClient.crosshairTarget at the entity for this frame — so
// the triggerbot's target resolution sees it like any vanilla pick. What the
// triggerbot cannot see on its own is HOW FAR that hit is: its reach gate
// measures the real body, which may already be past the range while the
// ghost is well inside it. Returns the eye→ghost hit distance in blocks, or
// -1 when the crosshair is not on `entity`'s ghost right now.
double ghost_hit_distance(JNIEnv* env, jobject entity);

} // namespace dt::modules::backtrack
