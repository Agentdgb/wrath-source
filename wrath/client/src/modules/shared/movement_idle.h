#pragma once

// Shared stand-still predicate.
//
// Grim's MultiActionsC ("Clicked in inventory while moving") and MultiActionsD
// ("Closed inventory while moving") both reject a container action taken while
// the last PlayerInput the client sent still reported movement:
//
//     if (event.getPacketType() != CLICK_WINDOW) return;
//     if (player.serverOpenedInventoryThisTick) return;
//     boolean input = player.supportsEndTick() && player.packetStateData.knownInput.moving();
//
// where KnownInput.moving() is forward || backward || left || right || jump,
// and the sneaking arm of the same check counts shift on 1.21.9+ clients.
// Opening your own inventory sends no packet on 1.12+, so
// serverOpenedInventoryThisTick is never set for it and the gate always
// applies. Any module driving a container has to observe stillness first.
//
// This lives here rather than inside one module because AutoTotem and SilentAim
// already carry a copy each; a third would be one more place for the mapping to
// drift.

#include <jni.h>

namespace vanta::shared {

// True when the player is pressing none of forward/back/left/right/jump/sneak
// AND is not sprinting.
//
// Sprint is checked separately because Grim tracks it separately: it is an
// entity state carried by its own PlayerCommand packet, and it OUTLIVES the
// key going up. Vanilla only clears it inside tickMovement, on the tick where
// forward input goes away. That makes reading it do double duty -- it is the
// condition Grim actually tests, and observing it false is itself proof that a
// tick has run since the screen opened, which no count of render-thread pumps
// can guarantee on its own.
//
// Returns TRUE when the state cannot be read, so a caller that waits on this
// can never wedge on a host where the mapping did not resolve. `mc` is the
// MinecraftClient instance.
bool movement_is_idle(JNIEnv* env, jobject mc);

// Writes FALSE into the six movement bindings, so vanilla's next tick computes
// an all-false PlayerInput and sends it.
//
// This only ever writes FALSE, and there is deliberately no restore-by-writing-
// true counterpart: writing true would press forward, back, left, right, jump
// AND sneak at once, and there is no honest value to restore to anyway -- we
// cannot know which keys the player is still physically holding. Release by
// dropping the input filter instead and letting Windows key repeat re-press
// whatever is genuinely still down.
//
// Must be paired with a raised input filter, or key repeat undoes it within
// about 30 ms.
void suppress_movement(JNIEnv* env, jobject mc);

// Reads or writes one binding by role, reusing the same resolution as the gate
// above. movement_key_pressed returns false when the binding is unresolved.
enum : int { KEY_FORWARD = 0, KEY_BACK = 1, KEY_LEFT   = 2,
             KEY_RIGHT   = 3, KEY_JUMP = 4, KEY_SNEAK  = 5,
             KEY_SPRINT  = 6 };

bool movement_key_pressed(JNIEnv* env, jobject mc, int index);

// Holding a binding down is how a module supplies INPUT rather than a decision.
// It matters for more than tidiness: from 1.21.2 the client reports its input
// to the server in a PlayerInput packet, and Grim simulates movement from that
// report -- so setting an entity's state directly, without the binding that
// justifies it, desyncs the prediction from what the player actually does.
// Feed the binding and vanilla produces both the state and the packet.
void set_movement_key(JNIEnv* env, jobject mc, int index, bool pressed);

// Sprint reset.
//
// Vanilla clears the sprint inside PlayerEntity.attack() on a strong hit, and
// the client tells the server only by way of sendMovementPackets noticing that
// isSprinting() no longer matches what it last sent. A module that HOLDS the
// sprint binding down destroys that signal without meaning to: doAttack runs
// inside handleInputEvents, so tickMovement re-reads the still-pressed key
// later in the SAME tick and turns the sprint straight back on. By the time
// sendMovementPackets compares, nothing has changed and no packet goes out --
// yet the attack really did multiply the player's velocity by 0.6. The server
// keeps crediting a full sprint (Grim adds 30% to its predicted speed while
// hasSprintingAttributeEnabled stands) against a client that just slowed down,
// and the gap surfaces as Simulation.
//
// Calling this from inside the attack, before tickMovement runs, releases the
// binding in time for vanilla's own reset to survive the tick and reach the
// wire as STOP_SPRINTING. The short window afterwards stops a holder from
// re-arming it too eagerly; one tick is all the packet needs.
//
// Only a MODULE-held binding is released (see sprint_key_owned). The player's
// own press is left alone: with "Sprint: Toggle" the binding is a
// StickyKeyBinding whose `pressed` is the toggle itself and a key release never
// clears it, so writing false there untoggled the player's sprint after every
// sprint hit. Vanilla re-arms a player-held sprint on the next tick anyway, and
// Grim models that attack slowdown on its own.
void request_sprint_reset(JNIEnv* env, jobject mc);

// True while set_movement_key last pressed the sprint binding on a module's
// behalf, i.e. the module -- not the player -- is the one holding it down.
bool sprint_key_owned();

// True while a reset requested above is still in effect. A module holding the
// sprint binding must consult this and leave the key alone.
bool sprint_reset_active();

} // namespace vanta::shared
