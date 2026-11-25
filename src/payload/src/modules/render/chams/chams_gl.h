// chams_gl.h — native OpenGL interception layer for the Chams module.
//
// WHY THIS EXISTS
// ---------------
// The overlay path (chams_draw.cpp) reconstructs the player's geometry and
// fills it on ImGui's foreground list. That works, but it can never draw the
// player's REAL appearance, because an overlay has no access to the skin's
// per-face UVs through a perspective-correct rasteriser.
//
// This layer takes the other road: let MINECRAFT draw the player, exactly as it
// always does, and defeat the depth test around that draw. The pixels are then
// the game's own — real skin, real armour, real everything — just visible
// through terrain.
//
// WHAT IS ACTUALLY VERIFIED, AND HOW
// ----------------------------------
// The central mechanism was confirmed empirically (a probe that created a GL
// context and compared resolution paths on this machine's NVIDIA driver), not
// assumed:
//
//   * wglGetProcAddress and GetProcAddress(opengl32) return DIFFERENT addresses.
//     glDrawElements resolves to nvoglv64.dll via wgl, and to a different stub
//     in OPENGL32.DLL via GetProcAddress. LWJGL calls nwglGetProcAddress FIRST
//     (disassembled from the lwjgl-opengl jar the 1.21.11 manifest pulls), so
//     the ICD address is the one Minecraft actually calls. Hooking the
//     opengl32 export intercepts NOTHING and fails silently.
//
//   * glDrawElementsBaseVertex, glDrawElementsInstanced,
//     glDrawElementsInstancedBaseVertex, glDrawArraysInstanced, glUseProgram
//     and glActiveTexture have NO opengl32 export at all — wgl only.
//
//   * The inverse holds for fixed-function state: glDepthFunc, glDepthMask,
//     glEnable, glDisable and glGetIntegerv return NULL from wglGetProcAddress
//     and must come from opengl32.
//
//   * Minecraft's entity shader is the only one of the 93 core shaders in the
//     1.21.11 jar containing the identifier `overlayColor`. That single string
//     classifies entity draws with zero Minecraft mappings involved.
//
// STAGED BRING-UP
// ---------------
// Stage 1 (what this file implements today) is INSTRUMENTATION ONLY. Every
// entry point is hooked and every classification is made and logged, but not a
// single GL state change is issued and every original function is called
// unmodified. The rendering is bit-identical to an un-hooked game. Its purpose
// is to answer, from a real session, the questions that cannot be answered by
// reading code:
//
//     - do the hooks fire at all, and on which entry points?
//     - does the `overlayColor` classifier actually identify entity programs?
//     - how many draws per frame are entity draws?
//     - which texture is bound when a player's body is drawn?
//     - what are the real depth func / depth mask values around those draws?
//
// Stage 2 adds the depth-defeated replay pass, and only once the Stage 1 log
// says the classification is sound. Building it the other way round means
// debugging a state-restore contract and a classifier at the same time, inside
// driver code, where a mistake is an access violation with no Java exception.
//
// SAFETY CONTRACT (applies to both stages)
// ----------------------------------------
//   * The hooks are armed ONCE and gated by an atomic. MinHook's enable/disable
//     suspends every thread in the process, so toggling the module must never
//     install or remove a hook.
//   * A re-entrancy guard suppresses BOTH the draw path and the state trackers
//     while Dt's own context is current. gl_hook.cc switches the same thread
//     between the game's context and ours, and ImGui's loader resolves to the
//     identical nvoglv64 stubs — without this, the client's own UI corrupts the
//     classifier state.
//   * State the chams pass will change (depth func, depth mask) is SHADOWED by
//     tracking the setters, never read back with glGetIntegerv inside a detour.
//     Blaze3D caches those values in Java fields and skips redundant GL calls,
//     so a restore that is off by one value breaks the world render for the
//     rest of the session with no error anywhere.
//   * No detour ever emits a GL call that could raise an error. Sodium enables
//     GL_DEBUG_OUTPUT_SYNCHRONOUS on NVIDIA, and this code runs in the GAME's
//     context, which gl_hook's quiet_debug_output() deliberately does not cover.

#pragma once

#include <cstdint>

namespace dt::modules::chams::gl {

// Arms the hooks. Must be called from the render thread with the GAME's context
// current — wglGetProcAddress needs a current context, which is why this cannot
// run from Bootstrap and is driven from the first frames instead. Idempotent;
// returns true once every required entry point is hooked.
bool install();

// Unhooks. Safe to call from shutdown; drains in-flight detours first.
bool uninstall();

bool installed();

// The module's own gate. Cheap atomic; the detours early-out on it. Toggling
// this is how the ClickGui enables and disables the feature — never by
// installing or removing hooks.
void set_active(bool on);
bool active();

// Raised by gl_hook.cc around the block where DT's overlay context is
// current. While raised, every detour passes straight through to the original
// without classifying, tracking or replaying anything.
//
// This is not an optimisation. ImGui's GL3 backend and the arraylist blur
// pipeline resolve their entry points through the same wglGetProcAddress path
// and therefore call the SAME nvoglv64 stubs this layer patches. Without the
// suppression the client's own UI draws would be classified as game draws and
// would corrupt current_program / bound-texture tracking.
void set_suppressed(bool on);

// Same, but returns the PREVIOUS value so a caller can restore it (RAII). Used by
// the ItemShader layer (item_shader_gl.cpp) to bracket its own GL without leaving
// suppression stuck on when it runs nested inside an already-suppressed section.
bool exchange_suppressed(bool on);

// Publishes the set of GL texture names that belong to player skins, resolved
// once per frame on the render thread over JNI. A draw is a PLAYER draw when
// its program is an entity program AND the texture bound to unit 0 is in this
// set. Passing count = 0 disables the narrowing (every entity draw matches).
void publish_player_textures(const std::uint32_t* names, int count);

// Of those, which are ARMOUR. Armour is exempt from the minimum-geometry filter:
// that filter exists to reject a placed head, and a boot and a skull are the
// same size. Texture is what tells them apart.
void publish_armour_textures(const std::uint32_t* names, int count);

// GL names an ENTITY draw bound that are in neither the player set nor the
// armour set. Empty is the healthy answer for armour: it means everything the
// game binds for equipment is something we resolved. A stable handful of ids
// here, appearing exactly when an armoured player is on screen, is the
// signature of equipment being rebound to a texture the vanilla asset path
// does not name -- the one failure mode our counters otherwise cannot see.
//
// Writes at most `cap` ids and returns how many. Diagnostics only.
// The busiest textures on entity draws, most-drawn first, with what we believe
// each one to be. This is the measurement that separates "the armour is drawn
// with a texture we do not recognise" from "the armour is recognised and lost
// later", which three previous rounds of counters could not.
//
// flags: bit 0 = in the player set, bit 1 = in the armour set, bit 2 = our own
// skin. Writes at most `cap` rows and returns how many.
int top_textures(std::uint32_t* ids, std::uint32_t* counts, unsigned char* flags, int cap);

int unaccounted_textures(std::uint32_t* out, int cap);

// The narrower and far more useful question: of the draws issued while an
// equipment feature renderer was on the stack -- a JNIHook bracket, so it is
// armour or a held item by construction and not by inference -- how many bound
// a texture we recognised, and how many did not? A high `unknown` count with a
// low `known` count is the signature of equipment being drawn with a texture
// that the vanilla asset paths do not name, and it is the one measurement that
// cannot be explained away by ordinary entities.
//
// Writes at most `cap` of the unrecognised ids and returns how many.
int equipment_unaccounted(std::uint32_t* out, int cap, unsigned long long& known,
                          unsigned long long& unknown);

// The local player's own skin texture. Draws that bind it get a DEPTH-ONLY
// replay into the band: they occlude other players' copies correctly without
// painting a chams on you. Pass 0 to disable.
void publish_self_texture(std::uint32_t name);

// Enable or disable the minimum-geometry filter.
//
// It rejects a placed player head, which wears a real player's skin and so
// cannot be told apart by texture. That works only where a player arrives as
// one large draw. From 1.21.9 the model is submitted part by part and player
// geometry overlaps a skull in size, so the filter is turned off there --
// keeping it deletes the player.
void set_size_filter(bool on);

// One-line-per-second summary into the module's desktop log, plus a counter
// dump. Called from the module's per-frame path so the log stays readable.
void log_frame_summary();

// Classifies any program seen by glUseProgram that has not been classified yet,
// by reading its shaders' GLSL back out of the driver.
//
// This exists because catching glShaderSource is NOT sufficient in practice:
// Minecraft compiles its core shaders during the resource reload, which is over
// long before a mid-game injection can hook anything. The first real session
// proved it — 593k draws intercepted, but only 2 shader sources ever seen and
// zero entity programs identified.
//
// glGetAttachedShaders + glGetShaderSource recover the source of an
// ALREADY-COMPILED shader, so classification does not depend on having been
// present at compile time.
//
// MUST be called at a frame boundary with the GAME's context current, never
// from inside a draw detour: it issues GL calls, and under Sodium's synchronous
// debug output a GL error is an in-driver abort. Doing it at the boundary also
// means the cost is paid once per program, not per draw.
void classify_pending_programs();

// Call once per frame, at the top of the swap detour. Rolls the per-frame
// counters over so the log can report DISTINCT entity textures drawn in the
// last frame.
//
// That number is the diagnostic that separates the two candidate explanations
// for "from some camera positions more or fewer players render":
//   * the game CULLED them  -> distinct textures drawn < players the JNI side
//                              sees in range, and the gap moves with the camera
//   * we FAILED TO CLASSIFY -> the game drew them, distinct textures is high,
//                              but the replay count does not follow
// Guessing between those two costs a day; counting them costs a frame.
void frame_boundary();

// Distinct entity textures observed during the previous complete frame.
int last_frame_entity_textures();

// Was this player-skin texture actually DRAWN by the game during the previous
// frame?
//
// This is the key to covering the one thing the replay approach cannot do on
// its own. A replay can only repeat a draw the game issues, and Minecraft culls
// entities it decides are not visible — measured in a real session, 32 players
// in range but only ~18 drawn, the count swinging with the camera. Those culled
// players simply have no draw to replay.
//
// Knowing WHICH skins were drawn means the module knows exactly which players
// the game dropped, and can fall back to the overlay for those alone. The two
// paths then cover each other's blind spots instead of competing.
bool player_texture_was_drawn(unsigned int gl_name);

// Was this texture bound at ANY entity draw during the previous frame —
// regardless of whether we classified that draw as a player's?
//
// Together with player_texture_was_drawn this separates the two ways a player
// can go missing, which demand opposite fixes:
//
//   skin seen at an entity draw, but not counted as a player draw
//       -> the game drew them; our MATCHING is wrong.
//   skin never seen at any entity draw
//       -> the id we resolved over JNI is not the id Minecraft binds. Our skin
//          RESOLUTION is wrong, and the player is invisible to the whole layer.
//
// The second is the live suspicion: several players share one texture id (the
// default skin), and "player skins resolved: 19 of 20" has been in every log.
bool entity_texture_was_seen(unsigned int gl_name);

// Replays issued during the previous complete frame.
int last_frame_replays();

// STAGE 2. When armed, a draw classified as a player is replayed once with
// glDepthFunc(GL_ALWAYS) and depth writes off BEFORE the game's own draw, so
// the occluded parts show through terrain while the visible parts stay exactly
// as the game rendered them.
//
// Off by default. This is the only call in the module that modifies the game's
// GL state, and it is the one that carries the real risk: the depth state is
// restored from the shadow the trackers maintain, and if that restore is ever
// wrong Blaze3D's cache silently disagrees with the driver for the rest of the
// session.
void set_replay(bool on);

// Paint the replayed player in one flat colour instead of their real skin,
// using the SAME mechanism as the normal mode: Minecraft's own draw, repeated
// with the depth test defeated. A white texture is bound so the shader emits
// plain white, and GL_CONSTANT_COLOR blending turns that into the chosen
// colour. The silhouette stays Minecraft's real animated geometry.
// Per-draw diagnostics. OFF by default: the texture bookkeeping and the
// per-draw log line each take a lock, on a path that runs millions of times a
// session. They found every bug in this module and cost real frames, so they
// are opt-in now rather than always-on.
void set_diagnostics(bool on);
// Whether the verbose diagnostics are on. Read by callers that want to do
// expensive work (a whole-registry walk) only when someone is looking.
bool diagnostics_on();

void set_flat_colour(bool on, float r, float g, float b, float a);
bool flat_colour_ready();
bool replaying();

// HOW THE WALL IS DEFEATED, and why it changed three times.
//
// The replay runs inside a compressed glDepthRange band of [0, 0.5], with the
// ordinary GL_LEQUAL test and depth writes ON. The band sits in front of all
// terrain, and INSIDE it the normal test still applies -- so the model sorts
// against itself correctly (torso over far arm, armour over body) while nothing
// in the world can occlude it.
//
// An intermediate version used glDepthFunc(GL_GREATER) with writes OFF, to
// paint only the fragments that fail the normal test. That is wrong for a
// reason worth keeping: a player's OWN geometry fails it too. The far arm is
// behind the torso and the body is behind its own armour, so with no depth of
// its own the copy had nothing to sort against and every part painted over the
// last. The result was a flat, shadeless silhouette with only a fringe of
// armour visible around the edge.
//
// The band's WIDTH then turned out to matter as much as its existence -- at
// [0, 0.01] the body and the armour collapsed onto the same depth value past
// about 20 m and z-fought into holes. See the table at the glDepthRange call.
//
// It previously squashed the replay into a compressed glDepthRange of
// [0, 0.01], which put the model in front of everything UNCONDITIONALLY. Two
// bugs came from that, and both were reported from the game rather than found
// here:
//
//   * The body showed through the player's own ARMOUR in the open, with no
//     block involved. Minecraft batches entity geometry per render layer and
//     flushes the batches in layer order, not submission order, so armour is
//     not reliably drawn after the body; when its layer flushed first, the
//     near-depth copy painted straight over it.
//   * Earlier, with depth writes forced ON, each replayed player stamped depth
//     into the shared band and hid every later player whose silhouette
//     overlapped -- which is why raising the camera appeared to "fix" it.
//
// The wide band answers both. Writes give the model an internal order, so the
// armour covers the body and the torso covers the far arm; the band's monotonic
// mapping keeps that order correct ACROSS players too, so a nearer player wins
// over a farther one exactly as it should -- the earlier "players hide each
// other" reading of that was wrong, and the real cause of the vanishing players
// was tr7zw's EntityCulling, which chams_cull.cpp now defeats.
//
// THE REMAINING COST, stated because it is real and visible: depth writes leave
// the band's value in the buffer, so anything drawn afterwards at those pixels
// that is NOT itself replayed fails LEQUAL and is punched out. That is why the
// replay set has to cover everything belonging to a player -- skins AND armour
// (chams_jni.cpp resolves both). Held items and particles are not yet in it.

// Skip replaying BLENDED draws. Replaying one composites the entity twice
// through the blend equation, which smears the translucent layers (glint,
// elytra, translucent trims). The opaque body is not blended, so the chams
// itself loses nothing. Default true.
void set_skip_blended(bool on);

// How many chams draw detours are on the stack RIGHT NOW, across all threads.
// Read by the crash reporter to decide whether a fault is plausibly ours: the
// JVM raises access violations as normal business, so "was a chams detour
// running?" is what separates our crash from its bookkeeping.
int in_detour();

// Detour depth ON THE CALLING THREAD. This is the one the crash reporter must
// use: the process-wide count says only that SOME thread is in a detour, and
// the JVM raises access violations on its own threads as normal business.
int in_detour_this_thread();

// Published by the per-entity cull hook (chams_cull.cpp) from
// EntityRenderDispatcher.shouldRender, which the game calls once per entity
// immediately before rendering it. Exact about WHICH ENTITY the game is about
// to render -- and, on its own, useless for deciding what to replay: it is only
// called for entities, so once the last one of a frame is done it stays raised
// over block entities, particles and the enchantment glint.
void set_rendering_player(bool on);

// Raised by chams_item.cpp for the duration of a player's equipment feature
// renderers -- HeldItemFeatureRenderer.render AND ArmorFeatureRenderer.render --
// with Minecraft's immediate-mode buffer flushed on BOTH sides of it -- so
// while this is true, the geometry actually reaching the driver is that item
// and nothing else. The flushes are what make the timing true instead of
// guessed; see chams_item.h for the three attempts that guessed.
void set_rendering_equipment(bool on);

// Same idea, but for a bracket whose OWNERSHIP IS ALREADY PROVEN.
//
// On 1.21.9+ the bracket does not wrap a feature renderer -- it wraps the
// emission of a batching queue that only this client put geometry into. Getting
// there required the submission to have passed the player test at SUBMIT time,
// so asking again at emit time is not a second check, it is a bug: the queue is
// emitted AFTER the entity pass, when the per-entity player flag holds whatever
// the last entity of the frame left behind. That stale read is what made
// enchanted items vanish -- an enchanted item is two draws, and the glint
// carries no player skin, so it had nothing to fall back on.
void set_rendering_owned_equipment(bool on);

// Read back by chams_item.cpp on the 1.21.9+ path, where the decision about
// whether a queued submission belongs to a player has to be made at SUBMIT
// time, long before any GL draw exists to classify.
bool rendering_player();

// ── FIRST-PERSON HELD-ITEM SHADER ────────────────────────────────────────────
//
// The item-shader feature lives in its OWN layer (item_shader_gl.h /
// item_shader_gl.cpp). The Chams draw layer only calls into it: the classifier
// hands each first-person-window draw to itemshader::consider_fp_draw, the draw
// macro replays the armed item via itemshader::run_capture, and frame_boundary
// drives itemshader::on_frame_boundary. Nothing item-shader-specific is declared
// here any more — see item_shader_gl.h.

} // namespace dt::modules::chams::gl
