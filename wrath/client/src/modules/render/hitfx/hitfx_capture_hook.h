// hitfx_capture_hook.h — captures the game's real world-render matrices WITHOUT
// hooking the render method.
//
// Hooking WorldRenderer.render via JNIHook works (the matrices come through) but
// JVMTI RetransformClasses on that hot render method breaks the world render on
// MC 1.21.7+ (black screen — the same failure the triggerbot notes document for
// tickMovement). So instead of intercepting the render, we READ MC's own render
// state each frame from the SwapBuffers thread:
//   * projection = GameRenderer.getBasicProjectionMatrix(getFov(...)) — MC's
//     exact projection matrix, read field-by-field (org.joml.Matrix4f m00..m33).
//   * modelview  = RotX(pitch)·RotY(yaw+180) built from Camera.pitch/yaw — the
//     exact first-person view transform MC's renderWorld applies.
//   * camera pos = Camera.pos (Vec3d).
// This reproduces Breeze's captured matrices (minus view-bob) with zero
// retransform, so nothing about the game's rendering is touched.

#pragma once

#include <jni.h>

namespace vanta::modules::hitfx::capture {

// Resolves all reflection IDs (classes/methods/fields). Idempotent. Returns true
// once everything resolved. Safe once a world is loaded (GameRenderer/Camera
// exist). Retried from the render pass until it succeeds.
bool install();
void shutdown();
bool is_installed();

// Reads MC's current projection + view + camera and publishes a render_frame.
// Called once per frame from the render pass (same thread, has a JNIEnv).
void capture_now(JNIEnv* env, bool force = false);

// Kept for API compatibility (the module still calls it); a no-op switch that
// gates capture_now's work.
void set_capture_enabled(bool on);

unsigned long long debug_fire_count();

} // namespace vanta::modules::hitfx::capture
