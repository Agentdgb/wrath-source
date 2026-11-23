// chams_item.h — bracket a player's HELD ITEM so its draw can be recognised.
//
// THE PROBLEM THIS SOLVES, AND WHY THE EASY ROUTES FAILED
// ------------------------------------------------------
// A held sword has no identity at the OpenGL layer. It is drawn from the shared
// block/item atlas -- the very same GL texture as a sword lying on the ground,
// an item frame, or terrain -- so the texture test that makes skins and armour
// exact has nothing to bite on. Three attempts went wrong before this one, and
// each failure is worth keeping:
//
//   1. DRAW-ORDER PROXIMITY. "An entity draw a few draws after a player's own
//      geometry is that player's item." False: Minecraft batches entity
//      geometry by render layer and flushes on a layer CHANGE, so the sword is
//      not adjacent to the body holding it. Measured: 25266 non-entity draws
//      replayed, which on screen was a field of blocks, and still no sword.
//
//   2. RESTORING THE DEPTH BUFFER after the replay, so the later item draw
//      would not be deleted by our own footprint. It works, and it destroys the
//      one thing the depth band exists for -- cross-draw ordering -- so the
//      armour went back to sorting by luck.
//
//   3. THE PER-ENTITY FLAG from EntityRenderDispatcher.shouldRender. Exact
//      about entities, unusable for draws: shouldRender is only called for
//      entities, so once the last one is done the flag stayed raised over block
//      entities, particles and the enchantment glint. Measured 379404 "parts"
//      against 394629 player draws, and the glint quad -- replayed into the
//      band -- covered the screen while players drawn afterwards failed the
//      depth test and vanished.
//
// WHAT THIS DOES INSTEAD
// ----------------------
// It stops guessing WHEN the item is drawn and makes the timing true, by
// hooking HeldItemFeatureRenderer.render and bracketing it with two flushes of
// Minecraft's own immediate-mode buffer:
//
//     draw()            <- flush everything already pending, flag DOWN
//     flag = up
//     original render   <- the item's geometry is buffered
//     draw()            <- flush; ONLY the item is in flight, flag UP
//     flag = down
//
// The first flush is what makes it exact. Without it the second would also push
// whatever earlier entities had left buffered, and the flag would once again be
// claiming things it does not own -- the same mistake as attempt 3, wearing a
// different hat.
//
// COST, stated plainly: this changes WHEN Minecraft submits geometry. Two extra
// buffer flushes per held item per frame means more, smaller draw calls, and it
// can shift translucency ordering. That is a real price and it is the reason
// this was the last option tried rather than the first.
//
// VERSION COVERAGE (read out of the repo's own cached Yarn tables, not assumed):
//   class  net/minecraft/class_989   identical across 1.21 .. 1.21.11
//   method method_17162 ("render")   four distinct descriptors over that range,
//                                    all four probed below
//   flush  class_4597$class_4598.method_22993 ("Immediate.draw")
//
// On 1.21.9+ the second parameter is class_11659 rather than a
// VertexConsumerProvider, so the instance check simply fails and the flush is
// skipped -- the hook degrades to doing nothing rather than to doing damage.

#pragma once

#include <jni.h>

namespace vanta::modules::chams::item {

// Idempotent; retried by the caller until a world exists. Returns true once the
// detour is in place.
bool install(JNIEnv* env);
bool installed();

// True only when BOTH halves of the armour path took: the submitModel redirect
// and a command renderer to bracket it. chams.cpp uses it to decide whether the
// shared armour texture ids still need to be folded into the player set, which
// is the crutch that also puts the chams on the local player and on armoured
// mobs. Never guess this -- if either half failed, the crutch must stay.
bool armour_queue_armed();

// Gates the whole thing. When false the detour still runs but only calls
// through, so toggling the module never installs or removes a Java hook.
void set_active(bool on);

// Close the frame and reconcile it. A frame that steered a player's equipment
// into our queue and never saw that queue emitted is the exact shape of "the
// item vanished", and it is counted here so the log can say whether that is
// what is happening instead of leaving it to be guessed at.
void on_frame();

void log_summary();
void shutdown();

} // namespace vanta::modules::chams::item
