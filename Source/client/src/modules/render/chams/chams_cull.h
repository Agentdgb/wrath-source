// chams_cull.h — defeat Minecraft's per-entity render culling for players.
//
// WHY
// ---
// The native replay path (chams_gl.h) can only repeat a draw the game actually
// issues. A real session measured the problem exactly:
//
//     drawn=12 | notDrawn_INSIDE=11 | notDrawn_outside=2 | behind=2
//     verdict: players INSIDE the view were not drawn
//     p00 d=21 skin=4565 ndc=(+0.13,+0.12) w=+19.1 INSIDE-FRUSTUM
//     p01 d=27 skin=4589 ndc=(-0.05,+0.09) w=+26.7 INSIDE-FRUSTUM
//
// Eleven players sitting near the centre of the screen, in front of the camera,
// at 21-28 blocks, with their skins resolved — and the game issued no draw for
// any of them. The count also rose sharply whenever the camera gained height,
// which is the signature of OCCLUSION culling: entities hidden behind blocks
// are dropped before they ever reach the renderer.
//
// That is the worst possible case for a chams, because a player behind a wall
// is precisely the one the feature exists to show. No amount of work in the GL
// layer can fix it: there is no draw call to intercept.
//
// WHAT THIS DOES
// --------------
// Hooks the game's own per-entity visibility decision and forces it to YES for
// players. Minecraft then submits their geometry as usual and the GL layer's
// replay covers them like any other.
//
// TARGET
// ------
//     class_898 (EntityRenderDispatcher) . method_3950
//     (Lnet/minecraft/class_1297;Lnet/minecraft/class_4604;DDD)Z
//     i.e. shouldRender(Entity, Frustum, camX, camY, camZ)
//
// Verified present, on the same class, with the same shape, in the cached Yarn
// mappings for ALL ten supported builds (1.21 through 1.21.11) — see
// tools/genmaps/.cache/<version>/yarn_merged.tiny. That makes it the most
// version-stable point in the whole chain.
//
// MECHANISM
// ---------
// vendor/jnihook, the same native-detour library the triggerbot already uses in
// production (src/modules/combat/triggerbot/triggerbot_hook.cpp). It turns the
// Java method into a native one whose original stays callable, so the detour can
// decide per entity instead of replacing the policy wholesale.
//
// SAFETY
// ------
//   * ONLY players are forced. Every other entity falls through to the game's
//     own answer, so mobs, item frames and dropped items keep their culling and
//     the extra rendering cost stays bounded by the player count.
//   * The attach is wrapped in SEH. Lunar Client and Feather apply aggressive
//     mixins to these classes, and JNIHook can fault while rewriting one — the
//     triggerbot hit exactly that and the same guard is used here. A failed
//     attach leaves the module working exactly as it does today rather than
//     taking the process down.
//   * If the hook is not installed the module is unchanged: this only ever adds
//     draws that were being skipped.

#pragma once

#include <jni.h>

namespace vanta::modules::chams::cull {

// Resolves the target and attaches the detour. Idempotent; safe to call every
// frame until it succeeds. Requires a live JNIEnv and a loaded world.
bool install(JNIEnv* env);

bool installed();

// Whether to actually force players visible. Off means the detour calls straight
// through to the game's own decision, so the feature can be toggled without ever
// detaching a hook.
void set_active(bool on);

// One line into the module's desktop log: how many decisions were seen, how many
// were overridden, and how many the game would have dropped.
void log_summary();

void shutdown();

} // namespace vanta::modules::chams::cull
