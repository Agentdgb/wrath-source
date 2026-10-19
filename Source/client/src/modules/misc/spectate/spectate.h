// spectate.h — see the world from another player's eyes, client-side.
//
// WHAT THIS IS
// ------------
// Minecraft renders the world from whatever entity MinecraftClient calls its
// CAMERA ENTITY, and that is a plain setter (method_1504). Point it at another
// player and the game draws their view. Nothing is sent to the server.
//
// FREE LOOK, AND WHY IT IS A HOOK AND NOT A WRITE
// ------------------------------------------------
// The user looks around with their own mouse while the camera sits in the other
// player's head. The camera takes its orientation from the camera entity, so the
// first version of this wrote the local player's yaw/pitch ONTO the target every
// frame. That fought the game: every 20 Hz tick the client re-lerps a remote
// player's rotation toward what the server last sent, our write only landed at
// swap time — after that frame had already rendered — and the result was a
// visible pop twenty times a second. It also clobbered the pitch fields the
// chams read, so the target's head tilted with the mouse.
//
// Read out of the real 1.21 and 1.21.11 client bytecode (javap on the Modrinth
// jars), Camera.update (method_19321) does this in order:
//
//     setRotation(entity.getYaw(td), entity.getPitch(td))   <- where the entity's
//                                                               look ENTERS the camera
//     setPos(lerped entity position)
//     if third person:
//         if front view: setRotation(this.yaw + 180, -this.pitch)   <- derived from
//                                                                      the fields the
//                                                                      first call wrote
//         moveBy(-clipToSpace(4), 0, 0)
//     else if sleeping: setRotation(bedDirection - 180, 0)
//
// So the whole free look is one native detour on Camera.setRotation(FF)V
// (method_19325), installed with the same vendored JNIHook the triggerbot and
// the chams culler already use. While spectating, the detour swaps the incoming
// (yaw, pitch) for the LOCAL player's — live, at the exact moment the camera is
// built — and passes the third-person flip through untouched, because it is
// computed from the values we just substituted. The target entity is never
// written. There is nothing to fight and nothing to undo.
//
// THE TARGET IS PINNED, NOT RE-FOUND
// -----------------------------------
// The panel hands over a NAME from the tab list, and a name in it can belong to
// somebody a thousand blocks away with no entity loaded. The name is walked
// against the loaded players (throttled, it is a string compare per player), and
// once found the entity is held by GLOBAL REF and checked each frame by identity
// via world.getEntityById — one JNI call, no strings. A target that despawns,
// logs out or leaves render distance stops answering to its id, the camera comes
// home the same frame, and the request stays standing so it re-attaches if they
// come back. resolved() says which of those two states we are in.
//
// Intermediary names, grepped from tools/genmaps/.cache/<ver>/yarn_merged.tiny
// and identical on 1.21, 1.21.5 and 1.21.11:
//     class_310  method_1551  getInstance     method_1504 setCameraEntity
//                method_1560  getCameraEntity field_1687 world  field_1724 player
//     class_638  method_18456 getPlayers      method_8469 getEntityById (on class_1937)
//     class_1297 method_5628  getId           method_36454 getYaw  method_36455 getPitch
//     class_1657 method_7334  getGameProfile
//     class_4184 method_19325 setRotation(FF)V   field_18718 yaw F   field_18717 pitch F

#pragma once

#include <string>

struct JNIEnv_;
typedef JNIEnv_ JNIEnv;
class _jobject;
typedef _jobject* jobject;   // same shape jni.h gives it under C++

namespace vanta::modules::spectate {

// Ask to watch `name`. Takes effect on the next tick, once the name resolves to
// a loaded entity. Safe to call from the render thread with no JNI in hand.
void request(const std::string& name);

// Go back to your own eyes. Idempotent.
void stop();

bool        active();
std::string target();

/* @why the target can be requested and still not be watched.

   A name from the tab list is not an entity. Until it resolves — or after the
   entity it resolved to is gone — this reports false, and the panel says so
   rather than leaving a button that looks pressed and does nothing. */
bool resolved();

/* @the entity the camera is looking out of, or -1.

   Vanilla does not draw the focused entity in first person; a render module
   that synthesises players from the entity list (chams, outline) has no such
   rule and would draw the target wrapped around the camera at zero distance.
   Same pattern as chams' own self id. */
int camera_entity_id();

// One frame, on the game thread. Cheap when idle: one atomic read.
void tick(JNIEnv* env);

/* @the anticheat seam: keeping the server's picture of you intact.

   ClientPlayerEntity.sendMovementPackets is one big `if (!isCamera()) return;`
   — read out of the 1.21 and 1.21.11 bytecode — so while the camera sits in
   another player's head the client sends the server NO position packet at all.
   Vanilla otherwise sends one at least every 20 ticks even standing perfectly
   still (the tick counter reaching 20 is itself a reason to send), and Grim's
   BadPacketsR flags exactly that silence: `positions == 0` across a window of
   more than two seconds while transactions are still being answered.

   A detour on isCamera fixes it when nothing else has hooked the packet path.
   It does NOT when something has: JNIHook runs an "original" out of a renamed
   COPY of the class, and inside that copy the call to isCamera resolves to the
   copy's own pristine method — a sibling class, so the real class's native
   override never gets selected — and it answers honestly. The silent aim hooks
   sendMovementPackets, so on this client that is the normal case.

   So the packet path asks for the camera instead. Around the one call that
   builds the movement packet, borrow() points MinecraftClient.cameraEntity
   back at the local player and give_back() returns it to the target. It is a
   RAW field write, not setCameraEntity, so the renderer's camera-changed hook
   never fires; and it is bracketed inside a single synchronous call on the
   game thread, so nothing can observe the gap. */
bool lying();
void borrow_camera(JNIEnv* env);
void return_camera(JNIEnv* env);

/* @the external camera hold: Freecam rides these hooks instead of adding two.

   Freecam parks the camera in an entity of its own and has exactly spectate's
   two problems: the movement packets stop unless isCamera is lied to (or the
   camera lent back around the packet build), and the only place the mouse can
   enter the camera with no jitter and no frame of lag is inside the
   Camera.setRotation detour, where Camera.update hands the entity's look in.
   So rather than a second copy of both detours -- one more JNIHook attach on
   the same methods, which JNIHook cannot even do -- the module borrows them
   through this API.

   arm() resolves the cache and installs the hooks with no target, and reports
   what landed. The rotation override runs inside the detour for every
   non-flip setRotation while set; it gets the incoming (yaw, pitch) and may
   replace them (return true). The hold makes lying()/borrow/return and the
   isCamera lie behave as if spectate were attached, with the HELD entity as
   what the camera is returned to after a loan. Freecam owns the lifecycle of
   all three; spectate's own restore paths never touch them, and shutdown()
   clears them first so a module still enabled at teardown cannot be called
   out of a dead detour. The detour can still be lost mid-session to the
   1.21.7+ no-op trap (see the .cpp), so a holder polls free_look_available()
   each frame rather than trusting arm()'s answer. */
struct ArmState { bool cache; bool free_look; bool cam_lie; };
ArmState arm(JNIEnv* env);
bool     free_look_available();

using RotationOverride = bool (*)(JNIEnv* env, jobject camera, float& yaw, float& pitch);
void set_rotation_override(RotationOverride fn);   // nullptr clears

void hold_camera(JNIEnv* env, jobject camera);     // spectate takes its own global ref
void release_camera(JNIEnv* env);

// Spectate's OWN watch is active, attached or still pinned -- the camera is
// not free for somebody else to take.
bool holds_camera();

// Puts the camera back and removes the detour. Called from the gui teardown,
// because a client unloaded with the hook live would leave the game calling
// into freed memory on the next camera update.
void shutdown(JNIEnv* env);

} // namespace vanta::modules::spectate
