// outline_view.h — Minecraft's VIEW BOB, as a matrix.
//
// WHY THIS EXISTS
// ---------------
// The shared matrix-capture service publishes the camera's modelview and
// projection, and everything built on it projects correctly — while you stand
// still. Walk, and the traced contour slides off the figure and swims around it.
//
// The reason is that GameRenderer.bobView pushes an EXTRA transform after the
// camera's own, just for the world pass: a small rock and sway driven by the
// player's limb animation. The world is drawn through it; the captured
// modelview does not contain it. So the game draws the player at one place and
// anything projected from the captured matrices puts them at another, and the
// gap opens and closes in time with your footsteps — which is exactly what
// "the outline moves when I move" looks like.
//
// This is not a new discovery in this repo. chams.h records the same gap in
// its own build_camera and calls it "a known, bounded inaccuracy"; the
// PlayerESP is the one module that folds the bob in, and it has never had the
// symptom. This file is the Outline's route to the same correction.
//
// HOW IT IS OBTAINED
// ------------------
// Not reimplemented. bobView's formula involves limb swing, sprinting, the
// FOV effects setting and a handful of constants, and a reimplementation drifts
// from the real one on every version that touches any of them. Instead a fresh
// MatrixStack is handed to Minecraft's own bobView and the result read straight
// back out — so what is applied IS the game's bob, by construction, including
// on a version where the maths changed.
//
// The "View Bobbing" video option is honoured: with it off, bobView is a no-op
// and this reports no correction rather than applying an identity matrix and
// paying for it.
//
// Intermediary names verified against tools/genmaps/.cache/<ver>/yarn_merged.tiny
// for 1.21, 1.21.5 and 1.21.11 — identical descriptors across all three:
//     class_757    GameRenderer      method_3186  bobView (Lclass_4587;F)V
//     class_4587   MatrixStack       method_23760 peek
//     class_4587$class_4665  Entry   method_23761 getPositionMatrix
//     field_1773   MinecraftClient.gameRenderer
//     field_1690   MinecraftClient.options

#pragma once

#include <jni.h>

namespace dt::modules::outline::view {

bool ensure_cache(JNIEnv* env);
void release_cache(JNIEnv* env);

// Writes the bob transform, column-major, matching org.joml's layout and the
// capture service's matrices. Returns false when bobbing is off, unavailable,
// or anything failed — in which case `out` is untouched and the caller should
// project without it, which is what every previous version of this module did.
bool bob_matrix(JNIEnv* env, float out[16]);

} // namespace dt::modules::outline::view
