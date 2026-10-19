#include "chams_gl.h"
#include "chams_debug.h"
#include "chams_crash.h"
#include "item_shader_gl.h"    // the ItemShader feature's OWN layer (chams only calls into it)

#include <windows.h>
#include <gl/GL.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vanta::modules::chams::gl {

namespace {

// ── GL types the 1.1 SDK header does not carry ───────────────────────────────
using GLchar     = char;
using GLsizeiptr = ptrdiff_t;
using GLintptr   = intptr_t;

constexpr GLenum kTexture0     = 0x84C0;
constexpr GLenum kConstantColor = 0x8001;   // GL_CONSTANT_COLOR

// ── Entry-point signatures ───────────────────────────────────────────────────
using PFN_glDrawElements                    = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glDrawArrays                      = void (APIENTRY*)(GLenum, GLint, GLsizei);
using PFN_glDrawElementsBaseVertex          = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLint);
using PFN_glDrawElementsInstanced           = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
using PFN_glDrawElementsInstancedBaseVertex = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
using PFN_glDrawArraysInstanced             = void (APIENTRY*)(GLenum, GLint, GLsizei, GLsizei);
using PFN_glMultiDrawElementsBaseVertex     = void (APIENTRY*)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei, const GLint*);

using PFN_glShaderSource   = void (APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glAttachShader   = void (APIENTRY*)(GLuint, GLuint);
using PFN_glLinkProgram    = void (APIENTRY*)(GLuint);
using PFN_glUseProgram     = void (APIENTRY*)(GLuint);
using PFN_glActiveTexture  = void (APIENTRY*)(GLenum);
using PFN_glBindTexture    = void (APIENTRY*)(GLenum, GLuint);
using PFN_glDepthFunc      = void (APIENTRY*)(GLenum);
using PFN_glDepthMask      = void (APIENTRY*)(GLboolean);
using PFN_glDepthRange     = void (APIENTRY*)(GLdouble, GLdouble);
using PFN_glTexImage2D     = void (APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFN_glTexStorage2D   = void (APIENTRY*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_glBlendFunc      = void (APIENTRY*)(GLenum, GLenum);
using PFN_glBlendFuncSep   = void (APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
using PFN_glBlendColor     = void (APIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glGenTextures    = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glEnable         = void (APIENTRY*)(GLenum);
using PFN_glDisable        = void (APIENTRY*)(GLenum);

// Query entry points — used ONLY by the deferred classifier at a frame
// boundary, never from a draw detour. Resolved but not hooked.
using PFN_glGetAttachedShaders = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLuint*);
using PFN_glGetShaderSource    = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGetShaderiv        = void (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glIsProgram          = GLboolean (APIENTRY*)(GLuint);

// Depth state, read back from the driver. See the seeding note at the replay
// gate: waiting to observe a setter cannot work against a caching layer that
// legitimately never calls it.
using PFN_glGetIntegerv        = void (APIENTRY*)(GLenum, GLint*);
using PFN_glGetBooleanv        = void (APIENTRY*)(GLenum, GLboolean*);
using PFN_glGetFloatv          = void (APIENTRY*)(GLenum, GLfloat*);
using PFN_glIsEnabled          = GLboolean (APIENTRY*)(GLenum);

constexpr GLenum kGL_SCISSOR_TEST = 0x0C11;

constexpr GLenum kDepthFuncQ      = 0x0B74;   // GL_DEPTH_FUNC
constexpr GLenum kDepthWriteMaskQ = 0x0B72;   // GL_DEPTH_WRITEMASK
constexpr GLenum kDepthRangeQ     = 0x0B70;   // GL_DEPTH_RANGE

constexpr GLenum kShaderSourceLength = 0x8B88;

// ── Originals ────────────────────────────────────────────────────────────────
PFN_glDrawElements                    o_DrawElements = nullptr;
PFN_glDrawArrays                      o_DrawArrays = nullptr;
PFN_glDrawElementsBaseVertex          o_DrawElementsBaseVertex = nullptr;
PFN_glDrawElementsInstanced           o_DrawElementsInstanced = nullptr;
PFN_glDrawElementsInstancedBaseVertex o_DrawElementsInstancedBaseVertex = nullptr;
PFN_glDrawArraysInstanced             o_DrawArraysInstanced = nullptr;
PFN_glMultiDrawElementsBaseVertex     o_MultiDrawElementsBaseVertex = nullptr;

PFN_glShaderSource  o_ShaderSource = nullptr;
PFN_glAttachShader  o_AttachShader = nullptr;
PFN_glLinkProgram   o_LinkProgram = nullptr;
PFN_glUseProgram    o_UseProgram = nullptr;
PFN_glActiveTexture o_ActiveTexture = nullptr;
PFN_glBindTexture   o_BindTexture = nullptr;
PFN_glDepthFunc     o_DepthFunc = nullptr;
PFN_glDepthMask     o_DepthMask = nullptr;
PFN_glDepthRange    o_DepthRange = nullptr;
PFN_glTexImage2D    o_TexImage2D = nullptr;
PFN_glTexStorage2D  o_TexStorage2D = nullptr;
PFN_glBlendFunc     o_BlendFunc = nullptr;
PFN_glBlendFuncSep  o_BlendFuncSep = nullptr;
PFN_glBlendColor    q_BlendColor = nullptr;   // resolved, not hooked
PFN_glGenTextures   q_GenTextures = nullptr;  // resolved, not hooked
PFN_glEnable        o_Enable = nullptr;
PFN_glDisable       o_Disable = nullptr;

typedef void (APIENTRY* PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
PFN_glColorMask     o_ColorMask = nullptr;

// Not hooked — resolved for the deferred classifier's own use.
PFN_glGetIntegerv        q_GetIntegerv = nullptr;
PFN_glGetBooleanv        q_GetBooleanv = nullptr;
PFN_glGetFloatv          q_GetFloatv   = nullptr;
PFN_glIsEnabled          q_IsEnabled   = nullptr;
std::atomic<bool>        g_shadow_seeded{ false };

PFN_glGetAttachedShaders q_GetAttachedShaders = nullptr;
PFN_glGetShaderSource    q_GetShaderSource = nullptr;
PFN_glGetShaderiv        q_GetShaderiv = nullptr;
PFN_glIsProgram          q_IsProgram = nullptr;

// ── Gates ────────────────────────────────────────────────────────────────────
std::atomic<bool> g_installed{ false };
std::atomic<bool> g_active{ false };
std::atomic<bool> g_suppressed{ false };
std::atomic<int>  g_in_detour{ 0 };

// PER THREAD, and that distinction is not pedantry.
//
// g_in_detour above counts detours across the whole process, which is what the
// uninstall drain needs. The crash reporter needs something else entirely: "was
// THIS thread inside one of our detours when it faulted?" Asking the global
// counter meant any JVM fault on any thread got claimed as ours whenever a
// render thread happened to be mid-detour -- and the JVM faults constantly by
// design, using access violations for its implicit null checks. The result was
// CRASH sections in the log for faults that were not ours and not even fatal,
// printing the self-contradicting "inside a chams GL detour = YES (depth=0)".
thread_local int t_in_detour = 0;

// Re-entrancy: a detour that calls back into a hooked function (or that is
// re-entered by our own logging) must not classify twice.
thread_local int t_reentry = 0;

struct reentry_guard {
    bool ok;
    reentry_guard() : ok(t_reentry == 0) { ++t_reentry; }
    ~reentry_guard() { --t_reentry; }
};

// True when a detour should do nothing but call through.
inline bool passthrough()
{
    return !g_active.load(std::memory_order_relaxed)
        ||  g_suppressed.load(std::memory_order_relaxed);
}

// ── Classifier state ─────────────────────────────────────────────────────────
//
// Guarded by one mutex. Contention is nil in practice: shader compilation and
// linking happen during a resource reload, and the per-draw reads take the lock
// only to test set membership. The alternative — lock-free structures in driver
// code — buys nothing and costs correctness.
std::mutex g_mx;
std::unordered_set<GLuint> g_entity_shaders;    // shader objects whose GLSL matched
std::unordered_set<GLuint> g_entity_programs;   // programs with such a shader attached
std::unordered_map<GLuint, std::vector<GLuint>> g_program_shaders;
std::unordered_set<GLuint> g_player_textures;   // GL names of player skins (JNI route)

// GL names of textures uploaded at SKIN DIMENSIONS.
//
// The JNI route resolves a player's skin through
// SkinTextures.body().texturePath() -> TextureManager.getTexture() -> GL id, and
// a real session proved it returns ids Minecraft NEVER binds: every missing
// player was tagged "[skin NEVER bound -> RESOLUTION wrong]", and the published
// ids (4559 4560 4561 4571 ...) did not intersect the ids actually bound at
// entity draws (4550 4567 4568 4570 ...). On 1.21.9+ player skins arrive
// through the new AssetInfo system, and asking TextureManager for that path
// appears to hand back a freshly registered placeholder rather than the live
// texture.
//
// So identity is taken from the GPU side instead, where no Minecraft mapping
// can be wrong: a Minecraft player skin is a 64x64 texture (or 64x32 for the
// legacy layout). Recording which texture names were uploaded at those exact
// dimensions identifies skins without asking Java anything.
//
// It is a heuristic, not a proof — a mob texture that happens to be 64x64 would
// also match. That is a far smaller error than the current one, which misses
// almost every player.
std::unordered_set<GLuint> g_skin_sized_textures;

// ── LOCK-FREE CLASSIFICATION ─────────────────────────────────────────────────
//
// classify() runs on EVERY draw the game makes — measured at 1.5 million entity
// draws in 35 seconds — and it used to take the mutex above to test two set
// memberships. A mutex acquisition per draw on the render thread is the single
// most expensive thing this layer did.
//
// GL names are small dense integers, so a direct-indexed byte table answers the
// same question with one relaxed atomic load and no lock at all. The sets above
// remain the authority and are still used off the hot path (publishing,
// diagnostics); these tables are their flattened mirror.
// Below this many indices a draw is too small to be part of a player. Both
// bounds are measured now rather than reasoned about:
//
//   * a real session logged minKept=108 as the SMALLEST draw belonging to a
//     player once armour joined the set (armour pieces are only a few cuboids,
//     so the floor dropped from the 432 of a bare player model),
//   * a placed player head is a skull model -- one cuboid plus its hat overlay,
//     2 x 6 quads x 6 indices = 72.
//
// 100 sat only eight indices under the observed floor, which is not a margin.
// 80 sits between the two measurements instead of hugging one of them.
constexpr GLsizei kMinPlayerIndices = 80;

// NO DRAW-ORDER HEURISTIC. It was tried and it was wrong.
//
// The idea was that an entity draw arriving a few draws after a player's own
// geometry had to be that player's held item. It does not hold: Minecraft
// batches entity geometry by RENDER LAYER and flushes the batches later, so a
// held sword is not adjacent to the body that is holding it. What the window
// actually caught was whatever happened to be drawn nearby -- a session logged
// equipOther=25266 non-entity draws replayed, which on screen was a field of
// blocks showing through walls while the sword still did not appear.
//
// A held item cannot be identified at this layer at all: it is drawn from the
// shared block atlas, the same GL texture as a dropped item, an item frame and
// the terrain. Texture identity -- the thing that makes skins and armour exact
// -- has nothing to bite on.
//
// So the item is not chased. The reason it was DISAPPEARING is addressed
// instead, at its root: see the depth-restore pass in the replay.

constexpr GLuint kMaxProgId = 4096;
constexpr GLuint kMaxTexId  = 32768;
std::atomic<unsigned char> g_prog_is_entity[kMaxProgId] = {};
// Programs whose vertex shader declares `in vec3 Normal` — i.e. lit 3D geometry:
// entities, blocks AND held item/block models, but NOT the 2D GUI/HUD shaders.
// The item-shader outline keys off this so a held BLOCK (a bed, etc.) — which is
// NOT an "entity" program (no overlayColor) and so was invisible to the entity
// filter — still gets outlined, while the HUD in the same window does not.
std::atomic<unsigned char> g_prog_has_normal[kMaxProgId] = {};
std::atomic<unsigned char> g_tex_is_player[kMaxTexId]   = {};

// ARMOUR, specifically -- a subset of the above.
//
// The minimum-geometry filter exists to reject a placed player head, which is a
// skull model wearing an online player's skin and therefore indistinguishable
// from a player by texture alone. It is NOT meant to touch armour, and a
// session measured it rejecting 28136 draws with maxRejected=72: two cuboids,
// the size of a pair of boots. Size cannot separate those two -- a skull with
// its hat layer is also 72 -- but their TEXTURES can, and the armour set is
// already resolved by name in chams_jni.cpp. So armour skips the filter and
// only skin-textured draws are measured against it.
std::atomic<unsigned char> g_tex_is_armour[kMaxTexId]   = {};

// OUR OWN SKIN. Deliberately NOT in the player set, and not unused either.
//
// The complaint was "I see other players through my own body in F5", and it is
// worth being precise about the cause: the chams is not being applied to you.
// Every other player's copy is replayed into the band, the band sits in front
// of ALL real geometry, and your own body is real geometry. Nothing is
// misbehaving -- that is what "in front of everything" means.
//
// So the answer is not to take something away from you, it is to give your body
// a PLACE in the band. Its draw is replayed there with colour writes masked
// off: depth only. Other players' copies then sort against you correctly, and
// you still see yourself painted by the game's own untouched draw.
std::atomic<unsigned int> g_self_tex{ 0 };
std::atomic<unsigned long long> c_self_depth{ 0 };

// How many replayed draws actually had an ARMOUR texture bound. "The armour
// does not render" has had several different causes already, and this is the
// one number that separates "it is never replayed" from "it is replayed and
// something later removes it". Guessing between those has cost several rounds.
std::atomic<unsigned long long> c_armour_draws{ 0 };

// -- TEXTURES AN ENTITY DRAW BOUND THAT WE CANNOT NAME -----------------------
//
// Armour is recognised by ID: chams_jni.cpp asks TextureManager which GL name
// belongs to minecraft:textures/entity/equipment/humanoid/iron.png and the draw
// path matches against the answer. That question is only ever as good as the
// assumption behind it -- that the game binds the texture the vanilla asset
// path names. When something rebinds equipment to a DIFFERENT texture, the id
// is simply not in our set, the draw is classified Entity instead of Player,
// and nothing is replayed. From the inside that is indistinguishable from the
// armour never being drawn: every counter we have reads the same either way.
//
// So record the ids we could not account for. Paired with the registry dump on
// the JNI side, an unknown id gets a NAME -- which is the difference between
// knowing what is happening and guessing at it. Two relaxed atomic ops on a
// path that already does several, and only for ids never seen before.
constexpr int kMaxUnknown = 24;
std::atomic<std::uint32_t> g_tex_draws[kMaxTexId] = {};

// See the note at the bail-out in observe_and_maybe_replay. These answer whether
// the armour fails the TEXTURE test or the SHADER test, which no counter so far
// has been able to tell apart.
std::atomic<unsigned long long> c_armour_any_draw{ 0 };
std::atomic<unsigned long long> c_armour_other_prog{ 0 };
// Programs whose fragment shader assigns gl_FragDepth. For these the depth
// range -- the entire mechanism the replay relies on -- is inert.
std::atomic<unsigned char> g_prog_fragdepth[kMaxProgId] = {};
std::atomic<unsigned long long> c_armour_fragdepth{ 0 };
std::atomic<unsigned long long> c_replay_fragdepth{ 0 };

// Programs that had an armour texture bound AND were NOT in the entity set.
// Each one gets ONE dump of its full vertex + fragment source, so the log
// carries the actual GLSL responsible for the armour path Vanta cannot see.
std::atomic<unsigned char> g_prog_source_dumped[kMaxProgId] = {};
std::atomic<int> g_source_dumps_left{ 16 };

// Which framebuffer is bound when a player or armour draw happens. If armour
// consistently reports a non-zero FBO while skin reports zero, the armour is
// being drawn into an offscreen target that our replay copies land in and the
// screen never sees. See the note where this counter is read.
std::atomic<unsigned long long> c_player_fbo0{ 0 };
std::atomic<unsigned long long> c_player_fboN{ 0 };
std::atomic<unsigned long long> c_armour_fbo0{ 0 };
std::atomic<unsigned long long> c_armour_fboN{ 0 };
std::atomic<std::uint32_t> g_last_armour_fbo{ 0 };

using PFN_glGetIntegervForFbo = void (APIENTRY*)(GLenum, GLint*);
PFN_glGetIntegervForFbo q_GetIntegervFbo = nullptr;
constexpr GLenum kDrawFramebufferBinding = 0x8CA9;
constexpr GLenum kFramebufferBinding     = 0x8CA6;
std::atomic<unsigned long long> c_player_fragdepth{ 0 };

std::atomic<unsigned char> g_prog_armour_seen[kMaxProgId] = {};
std::atomic<std::uint32_t> g_prog_armour[8] = {};
std::atomic<int> g_prog_armour_n{ 0 };
std::atomic<unsigned char> g_tex_unknown_seen[kMaxTexId] = {};
std::atomic<std::uint32_t> g_unknown_id[kMaxUnknown]     = {};
std::atomic<int>           g_unknown_n{ 0 };

// The same, but only for draws issued while an equipment feature renderer is
// running. This is the list that carries the answer.
std::atomic<unsigned char> g_tex_equip_seen[kMaxTexId] = {};
std::atomic<std::uint32_t> g_equip_id[kMaxUnknown]     = {};
std::atomic<int>           g_equip_n{ 0 };
// And the counterpart: equipment draws whose texture we DID recognise.
std::atomic<unsigned long long> c_equip_known{ 0 };
std::atomic<unsigned long long> c_equip_unknown{ 0 };

void note_unaccounted(GLuint t, bool in_equipment)
{
    if (!t || t >= kMaxTexId) return;
    const bool known = g_tex_is_player[t].load(std::memory_order_relaxed)
                    || g_tex_is_armour[t].load(std::memory_order_relaxed)
                    || t == g_self_tex.load(std::memory_order_relaxed);

    if (in_equipment) {
        (known ? c_equip_known : c_equip_unknown).fetch_add(1, std::memory_order_relaxed);
        if (!known) {
            unsigned char e2 = 0;
            if (g_tex_equip_seen[t].compare_exchange_strong(e2, 1,
                                                            std::memory_order_relaxed)) {
                const int s2 = g_equip_n.fetch_add(1, std::memory_order_relaxed);
                if (s2 < kMaxUnknown) g_equip_id[s2].store(t, std::memory_order_relaxed);
            }
        }
    }

    if (known) return;
    unsigned char expected = 0;
    if (!g_tex_unknown_seen[t].compare_exchange_strong(expected, 1,
                                                       std::memory_order_relaxed))
        return;
    const int slot = g_unknown_n.fetch_add(1, std::memory_order_relaxed);
    if (slot < kMaxUnknown)
        g_unknown_id[slot].store(t, std::memory_order_relaxed);
}

// ── WHY OTHER PLAYERS STILL SHOW THROUGH YOU IN THIRD PERSON ────────────────
//
// Not because the depth write is missing -- selfDepth proves it happens. The
// question is ORDER. Writing depth cannot un-paint somebody already painted, so
// only the players Minecraft emits AFTER our body can be occluded by it. These
// two counters measure exactly that split, per frame, and settle whether order
// is the obstacle or something else is.
std::atomic<unsigned long long> c_players_before_self{ 0 };
std::atomic<unsigned long long> c_players_after_self{ 0 };
bool g_self_drawn_this_frame = false;

// The minimum-geometry filter, and whether it is allowed to run at all.
//
// It was measured on 1.21.4, where Minecraft submits a whole player as ONE draw
// of 432 indices and a placed head as 72. That gap is what the threshold sits
// in, and it does not exist on 1.21.9+: there the model is submitted PART BY
// PART through submitModelPart, so a head, a torso, an arm each arrive as their
// own draw of one or two cuboids.
//
// A session on 1.21.11 measured it exactly: minKept=24 against maxRejected=72.
// Real player geometry at 24 indices, a skull at 72 -- the two ranges OVERLAP,
// so no threshold separates them and the filter was deleting the player model
// itself. That is what "the armour does not render, like it does not exist"
// was, and what put a held item over a face: the HEAD is under 80 indices too,
// so it never reached the depth band to occlude anything.
//
// Where the premise holds the filter still earns its place. Where it does not,
// it is switched off rather than retuned, because there is no value to retune
// it to.
std::atomic<bool> g_size_filter{ true };

// Names currently flagged in g_tex_is_player, so the next publish can clear
// exactly those rather than wiping 32 KB.
std::vector<GLuint> g_flagged_player_tex;
std::vector<GLuint> g_flagged_armour_tex;

inline void flag_entity_program(GLuint p)
{
    if (p < kMaxProgId) g_prog_is_entity[p].store(1, std::memory_order_relaxed);
}

// ── DIAGNOSTICS GATE ─────────────────────────────────────────────────────────
//
// The per-draw texture bookkeeping and the per-draw log line were what found
// every bug in this module, and every one of them takes a lock. With the module
// working they are pure overhead on the render thread, so they are off by
// default and can be turned back on when something needs investigating again.
std::atomic<bool> g_diag{ false };

// Every program id glUseProgram has ever bound, and the subset already put
// through the deferred classifier. The difference is the work queue.
std::unordered_set<GLuint> g_seen_programs;
std::unordered_set<GLuint> g_classified_programs;

// Shadowed GL state. Written by the setter trackers, read by the (Stage 2)
// chams pass so it can restore byte-exact WITHOUT a glGetIntegerv inside the
// detour. Blaze3D caches these in Java fields and skips redundant GL calls, so
// restoring a wrong value silently breaks the world render for the session.
// Size filter evidence. A threshold that silently rejected everything would
// look exactly like "the chams stopped working", so the numbers get logged.
std::atomic<unsigned long long> c_reject_small{ 0 };
std::atomic<int> c_min_kept{ 1 << 30 };
std::atomic<int> c_max_rejected{ 0 };

// Shadowed like every other piece of state the replay touches. Reading it back
// with glGetIntegerv inside a detour is not affordable, and restoring a value
// the game never set is destructive rather than merely inaccurate.
std::atomic<unsigned char> g_colour_mask{ 0x0F };   // RGBA bits, all on
std::atomic<unsigned long long> c_colourmask_calls{ 0 };

// Set by the per-entity cull hook. See set_rendering_player in the header.
std::atomic<bool> g_rendering_player{ false };
std::atomic<bool> g_rendering_equipment{ false };
std::atomic<bool> g_rendering_owned{ false };      // ownership proven at submit time
std::atomic<unsigned long long> c_held_item_draws{ 0 };
std::atomic<unsigned long long> c_held_via_skin{ 0 };

// ── FIRST-PERSON HELD-ITEM SHADER — hand-off to its own layer ─────────────────
// The whole feature (state, GL programs, capture, blur, composite) lives in
// item_shader_gl.cpp. The one thing the Chams draw MACRO needs is this flag: set
// by the classifier (observe) when the current draw is the armed first-person
// item, read by the macro to replay it via itemshader::run_capture. Thread-local:
// macro and observe share the render thread.
thread_local bool t_item_outline = false;

std::atomic<unsigned long long> c_depth_restores{ 0 };

std::atomic<GLenum>    g_depth_func{ GL_LEQUAL };
std::atomic<GLboolean> g_depth_mask{ GL_TRUE };
// glDepthRange defaults to [0,1]. Shadowed for the same reason as the other
// two: the replay changes it and must put back exactly what was there.
std::atomic<double>    g_depth_near{ 0.0 };
std::atomic<double>    g_depth_far { 1.0 };
// Whether the depth test and blending are currently enabled. Blending matters
// because replaying a BLENDED draw composites the entity twice, which is
// visibly wrong on translucent layers (glint, elytra, water-logged parts).
std::atomic<bool>      g_depth_test_on{ true };
std::atomic<bool>      g_blend_on{ false };
// Blend factors, shadowed for the same reason as the depth state: the flat
// colour pass changes them and must put back exactly what the game had.
// All FOUR factors. Blaze3D drives blending through glBlendFuncSeparate, not
// glBlendFunc — shadowing only the two-argument form left this permanently at
// the hardcoded defaults below, and "restoring" those over the game's real
// state is what turned the world black. Same failure as glBindTexture and the
// depth setters before it: the wrong entry point was hooked, the shadow never
// updated, and writing it back was destructive rather than merely inaccurate.
std::atomic<GLenum>    g_blend_src_rgb{ GL_SRC_ALPHA };
std::atomic<GLenum>    g_blend_dst_rgb{ GL_ONE_MINUS_SRC_ALPHA };
std::atomic<GLenum>    g_blend_src_a  { GL_ONE };
std::atomic<GLenum>    g_blend_dst_a  { GL_ONE_MINUS_SRC_ALPHA };
std::atomic<uint64_t>  c_blendfunc_calls{ 0 };

// A 1x1 opaque white texture, created lazily at a frame boundary.
//
// It is what makes the colour mode FLAT rather than a tint. Bound in place of
// the skin, the entity shader samples pure white, so the fragment leaving the
// shader is white * vertexColour. Multiplying that by GL_CONSTANT_COLOR then
// yields the chosen colour itself instead of the colour times the skin.
// A 1x1 texture holding the CHOSEN colour, re-uploaded whenever the colour
// changes. Bound in place of a player's skin it makes the whole body that one
// colour, without going anywhere near the blend stage.
GLuint g_colour_tex = 0;
std::atomic<unsigned> g_colour_packed{ 0xFFFFFFFFu };  // last uploaded RGBA
std::atomic<bool>     g_colour_dirty{ true };

// Bound-texture tracking, per texture unit. Unit 0 is where MC binds the entity
// texture; the rest are tracked only so the active-unit bookkeeping is correct.
constexpr int kMaxUnits = 8;
std::atomic<int>    g_active_unit{ 0 };
std::atomic<GLuint> g_bound_tex[kMaxUnits] = {};
std::atomic<GLuint> g_current_program{ 0 };

// ── Counters (Stage 1 evidence) ──────────────────────────────────────────────
std::atomic<uint64_t> c_draws{ 0 };
std::atomic<uint64_t> c_entity_draws{ 0 };
std::atomic<uint64_t> c_player_draws{ 0 };
std::atomic<uint64_t> c_suppressed{ 0 };
std::atomic<uint64_t> c_shaders_seen{ 0 };
std::atomic<uint64_t> c_entity_shaders{ 0 };
std::atomic<uint64_t> c_programs_linked{ 0 };
std::atomic<uint64_t> c_entity_programs{ 0 };

// How many times each depth-state setter was actually observed. Zero means the
// shadow is still this file's hardcoded default, and the replay must refuse to
// run rather than "restore" a value the game never set.
std::atomic<uint64_t> c_depthfunc_calls{ 0 };
std::atomic<uint64_t> c_depthmask_calls{ 0 };
std::atomic<uint64_t> c_depthrange_calls{ 0 };

// Diagnosing tex0 = 0. The first session showed the bound-texture tracker
// permanently reading zero, which kills any per-player narrowing. There are two
// candidate explanations and they need different fixes, so both are counted
// rather than guessed at:
//   (a) the hook never fires  -> c_bind_calls stays 0
//   (b) it fires but the active unit is not 0 when the skin is bound
//       -> c_bind_calls climbs while unit0 stays 0, and c_bind_unit shows where
//          the binds are actually landing
std::atomic<uint64_t> c_bind_calls{ 0 };
std::atomic<uint64_t> c_active_calls{ 0 };
std::atomic<uint64_t> c_bind_unit[kMaxUnits] = {};

// Draws per entry point. Two questions this answers that nothing else can:
// which call Minecraft actually uses for entities on this version, and whether
// glMultiDrawElementsBaseVertex is being driven — that one is Sodium's terrain
// path, so a non-zero count is direct evidence Sodium is running, which in turn
// bears on whether entity culling is what makes players vanish.
enum EntryIdx {
    kEDrawElements = 0, kEDrawArrays, kEDrawElementsBaseVertex,
    kEDrawElementsInstanced, kEDrawElementsInstancedBaseVertex,
    kEDrawArraysInstanced, kEMultiDrawElementsBaseVertex, kEntryCount
};
std::atomic<uint64_t> c_entry[kEntryCount] = {};
std::atomic<uint64_t> c_entry_entity[kEntryCount] = {};

// Distinct textures observed bound to unit 0 at the moment of an ENTITY draw.
// This is the raw material the per-player narrowing needs: once these ids show
// up, matching them against the players' skin GL names becomes possible.
std::unordered_set<GLuint> g_entity_draw_textures;

// Same idea, but reset every frame: how many DISTINCT entity textures the game
// actually drew in one frame. Compared against the player count the JNI side
// sees in range, this is what tells culling apart from mis-classification.
std::unordered_set<GLuint> g_frame_entity_tex;

// Player-skin textures the game actually DREW, this frame and last. The "last"
// snapshot is what render_text consults to decide which players the game culled
// and therefore need the overlay fallback.
std::unordered_set<GLuint> g_frame_player_tex;
std::unordered_set<GLuint> g_last_frame_player_tex;
// Same, but for EVERY entity draw regardless of classification.
std::unordered_set<GLuint> g_last_frame_entity_tex_set;
std::atomic<int> g_last_frame_entity_tex{ 0 };
std::atomic<int> g_last_frame_replays{ 0 };
std::atomic<uint64_t> c_replays_at_frame_start{ 0 };

// ─────────────────────────────────────────────────────────────────────────────
// Draw classification. Called with the re-entrancy guard already held.
enum class DrawKind { Other, Entity, Player };

// Two relaxed atomic loads and no lock. See the tables above for why.
DrawKind classify()
{
    const GLuint prog = g_current_program.load(std::memory_order_relaxed);
    if (prog == 0 || prog >= kMaxProgId) return DrawKind::Other;

    const GLuint tex = g_bound_tex[0].load(std::memory_order_relaxed);

    // ARMOUR DECIDES ON ITS OWN. See the note above this function: measured on a
    // live session, 7559 of 9333 armour draws were running a program the entity
    // test rejects, and the armour vanished for that reason alone. These
    // textures have exactly one user in the game, so their identity is a
    // stronger signal than the shader heuristic that was overruling it.
    if (tex < kMaxTexId && g_tex_is_armour[tex].load(std::memory_order_relaxed))
        return DrawKind::Player;

    if (!g_prog_is_entity[prog].load(std::memory_order_relaxed)) return DrawKind::Other;

    const bool is_player = tex < kMaxTexId
                        && g_tex_is_player[tex].load(std::memory_order_relaxed) != 0;
    return is_player ? DrawKind::Player : DrawKind::Entity;
}

// Stage 2 gate. When false the layer only observes (Stage 1 behaviour) and the
// game's rendering is bit-identical to un-hooked. When true, a matching draw is
// replayed once with the depth test defeated BEFORE the real call.
std::atomic<bool> g_replay{ false };
// Flat-colour mode: paint the replayed player in one chosen colour instead of
// their real skin. Same mechanism as the normal mode — Minecraft's own draw,
// repeated with the depth test defeated — so it inherits every property that
// path already has, including EntityCulling compatibility.
std::atomic<bool>  g_flat_colour{ false };

// See set_magenta_diag / the note in the replay body.
std::atomic<bool>  g_magenta_diag{ false };
std::atomic<float> g_col_r{ 1.f }, g_col_g{ 0.2f }, g_col_b{ 0.2f }, g_col_a{ 1.f };
std::atomic<bool> g_skip_blended{ false };
std::atomic<uint64_t> c_replays{ 0 };
std::atomic<uint64_t> c_skipped_blend{ 0 };

// Observe, and — when the replay is armed — issue the through-wall pass.
//
// The replay draws FIRST, then the caller performs the game's own unmodified
// draw on top. So where the player is genuinely visible the pixels are the
// game's normal ones, and where they are occluded the replayed copy shows
// through. That ordering is what keeps the visible case looking untouched.
//
// The depth state is restored from the SHADOW maintained by the glDepthFunc /
// glDepthMask trackers, never from glGetIntegerv: Blaze3D caches those values
// in Java fields and skips redundant GL calls, so restoring a value it does not
// expect silently breaks the world render for the rest of the session. The
// restore calls go through the ORIGINALS so they do not disturb the shadow.
// `count` is the draw's index/vertex count, or -1 where the entry point makes
// it meaningless (glMultiDrawElementsBaseVertex). It is what separates a player
// from a player's severed head -- see the size filter below.
template <class DrawFn>
void observe_and_maybe_replay(const char* entry, int entry_idx, GLsizei count,
                              DrawFn&& redraw)
{
    c_draws.fetch_add(1, std::memory_order_relaxed);
    if (entry_idx >= 0 && entry_idx < kEntryCount)
        c_entry[entry_idx].fetch_add(1, std::memory_order_relaxed);

    // ── FIRST-PERSON HELD-ITEM SHADER ────────────────────────────────────────
    //
    // The feature lives in its own layer (item_shader_gl.cpp). The Animations
    // detour raised itemshader::rendering_firstperson at renderFirstPersonItem and
    // it stays up until the frame boundary (the window is WIDE on purpose: 1.21.9+
    // submits the item to a render-command queue that only emits the real glDraw
    // later in the frame). Here the classifier just hands the item shader each draw
    // seen inside that window, with the program classification WE already have
    // (is_entity / has_normal from our per-program tables). The item shader decides
    // whether the draw is the held item — and, via its HUD latch, keeps the hotbar
    // and inventory item models (3D and in the same window) from being captured.
    if (::vanta::modules::itemshader::rendering_firstperson()
        && count > 0
        && entry_idx != kEMultiDrawElementsBaseVertex) {
        const GLuint fp_prog = g_current_program.load(std::memory_order_relaxed);
        const bool is_entity = fp_prog && fp_prog < kMaxProgId
            && g_prog_is_entity[fp_prog].load(std::memory_order_relaxed);
        const bool has_normal = fp_prog && fp_prog < kMaxProgId
            && g_prog_has_normal[fp_prog].load(std::memory_order_relaxed);
        const GLuint fp_tex0 = g_bound_tex[0].load(std::memory_order_relaxed);
        if (::vanta::modules::itemshader::consider_fp_draw(
                fp_prog, (int)count, is_entity, has_normal, fp_tex0))
            t_item_outline = true;
    }

    // OUR OWN BODY: depth into the band, no colour.
    //
    // This runs before classification because the draw must not be treated as a
    // player's -- it is ours. It writes the band value our silhouette occupies
    // so that other players' replayed copies behind us fail LEQUAL and are
    // correctly hidden, which is the whole of the F5 complaint. Colour is
    // masked off, so nothing of the chams appears on us; the pixels you see are
    // the game's own draw, untouched, immediately afterwards.
    {
        const unsigned int self_tex = g_self_tex.load(std::memory_order_relaxed);
        if (self_tex && o_DepthRange && o_ColorMask
            && g_bound_tex[0].load(std::memory_order_relaxed) == self_tex
            && g_replay.load(std::memory_order_relaxed)
            && c_depthmask_calls.load(std::memory_order_relaxed) != 0) {

            const unsigned char cm = g_colour_mask.load(std::memory_order_relaxed);
            const GLboolean saved_mask = g_depth_mask.load(std::memory_order_relaxed);
            const double n0 = g_depth_near.load(std::memory_order_relaxed);
            const double f0 = g_depth_far.load(std::memory_order_relaxed);

            // PAINT FIRST, THEN WRITE THE BAND. The order is the whole fix.
            //
            // Writing the band value before the body was painted made the
            // player INVISIBLE, and by exactly the mechanism this module has
            // hit at every turn: the band sits in front, so the game's own
            // draw -- which the detour performs immediately after this returns
            // -- then failed LEQUAL against a value we had just put there
            // ourselves. Depth in front of a thing you have not drawn yet
            // deletes it.
            //
            // So the body is drawn here with the game's own state, untouched,
            // and only afterwards does the band value go down. The game's own
            // follow-up draw does fail against it, which is now harmless: the
            // pixels are already on screen from the draw just below.
            redraw();

            o_ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            o_DepthRange(0.0, 0.5);
            o_DepthMask(GL_TRUE);
            redraw();
            o_DepthMask(saved_mask);
            o_DepthRange(n0, f0);
            // Restore the game's OWN mask, never a hardcoded all-on: Minecraft
            // masks channels during several passes.
            o_ColorMask((cm & 1) ? GL_TRUE : GL_FALSE, (cm & 2) ? GL_TRUE : GL_FALSE,
                        (cm & 4) ? GL_TRUE : GL_FALSE, (cm & 8) ? GL_TRUE : GL_FALSE);
            c_self_depth.fetch_add(1, std::memory_order_relaxed);
            g_self_drawn_this_frame = true;
            return;
        }
    }

    const DrawKind k = classify();

    // ── A PLAYER'S HELD ITEM ─────────────────────────────────────────────────
    //
    // Two independent signals, and BOTH are required, because each alone has
    // already been measured being wrong:
    //
    //   g_rendering_equipment  chams_item.cpp brackets
    //                          HeldItemFeatureRenderer.render and flushes
    //                          Minecraft's buffer on both sides, so the geometry
    //                          arriving really is the item. But it fires for
    //                          EVERY living entity -- a zombie's sword too.
    //   g_rendering_player     the game said the entity being rendered is a
    //                          player. Exact about entities, and on its own it
    //                          stays raised over block entities and particles
    //                          once the last entity is done, which is how the
    //                          enchantment glint became a square over the screen.
    //
    // Their intersection is neither: inside one specific player's own item
    // render, with the buffer boundary forced to fall around it.
    // NOTE the missing `k != DrawKind::Player`. Requiring it is what made the
    // item flicker out of players' hands, and the log named the mechanism:
    //
    //     sizeFilter: rejected=34320  maxRejected=72
    //
    // Minecraft does not always rebind the texture between a player's body and
    // the item in their hand, so the skin is often still bound when the item is
    // drawn. classify() then answers Player -- correctly, by its own rule -- the
    // draw took the ordinary player path instead of this one, and the minimum
    // geometry filter (threshold 80) threw it out for being 72 indices. Not
    // replayed, and then deleted outright by the depth our own band had already
    // written in front of it. A miss did not cost the chams on the item, it cost
    // the ITEM.
    //
    // So the bracket decides, not the texture. Inside it, whatever arrives IS
    // the item.
    //
    // Ownership still has to be established, and either signal will do it:
    // the game confirmed a player is being rendered, or the texture bound is
    // that player's own skin. A zombie holding a sword satisfies neither.
    // Two brackets, and they are NOT interchangeable.
    //
    //   g_rendering_equipment  wraps a feature renderer, which runs for every
    //                          living entity -- a zombie with a sword included.
    //                          Ownership is still open, so the player test
    //                          applies.
    //   g_rendering_owned      wraps the emission of a batching queue that only
    //                          this client submitted into (1.21.9+). Ownership
    //                          was settled at SUBMIT time. Re-testing here reads
    //                          a flag left over from the entity pass, which by
    //                          then belongs to whatever entity happened to be
    //                          last -- and that is precisely what deleted
    //                          enchanted items.
    const bool owned_bracket = g_rendering_owned.load(std::memory_order_relaxed);
    const bool open_bracket  = g_rendering_equipment.load(std::memory_order_relaxed)
                            && (g_rendering_player.load(std::memory_order_relaxed)
                                || k == DrawKind::Player);
    bool held_item = false;
    if ((owned_bracket || open_bracket)
        && entry_idx != kEMultiDrawElementsBaseVertex
        && count > 0) {
        held_item = true;
        c_held_item_draws.fetch_add(1, std::memory_order_relaxed);
        if (k == DrawKind::Player)
            c_held_via_skin.fetch_add(1, std::memory_order_relaxed);
    }

    // The per-entity flag is NEVER acted on alone. It is still
    // published (chams_cull.cpp) because it is genuinely exact about WHOSE
    // entity is being rendered -- but exact is not the same as usable here, and
    // acting on it cost two visible bugs:
    //
    //   * shouldRender is only called for ENTITIES. Once the last one of the
    //     frame is done the flag stays raised, so block entities, particles and
    //     the enchantment glint all arrived looking like a player's equipment.
    //     The glint is a full quad; replayed into the band it became the square
    //     that appeared over everything.
    //   * those extra draws stamped near depth across the band, so players
    //     drawn afterwards failed LEQUAL and vanished. A session measured
    //     playerParts=379404 against player=394629 -- one to one, which no
    //     amount of held items could account for.
    //
    // There is a deeper problem than the missing off-switch, and it is why this
    // is not simply being bounded and retried: Minecraft batches entity
    // geometry and flushes it on a render-layer change, so a player's held item
    // is not guaranteed to reach the driver while that player's own flag is
    // still the one raised. The signal is exact about entities and unreliable
    // about draws.
    // Measured BEFORE the bail-out, deliberately: everything past this line
    // has already been filtered by the very test under suspicion.
    {
        const GLuint at = g_bound_tex[0].load(std::memory_order_relaxed);
        if (at < kMaxTexId && g_tex_is_armour[at].load(std::memory_order_relaxed)) {
            c_armour_any_draw.fetch_add(1, std::memory_order_relaxed);
            {
                const GLuint fp = g_current_program.load(std::memory_order_relaxed);
                if (fp < kMaxProgId && g_prog_fragdepth[fp].load(std::memory_order_relaxed))
                    c_armour_fragdepth.fetch_add(1, std::memory_order_relaxed);
            }
            // Still counts draws whose PROGRAM the entity test rejects -- the
            // fix in classify() rescues them, so this now reads as "how many
            // armour draws that rescue is carrying" rather than "how many are
            // being lost". Same measurement, and it must stay non-zero.
            const GLuint cp = g_current_program.load(std::memory_order_relaxed);
            if (cp >= kMaxProgId
                || !g_prog_is_entity[cp].load(std::memory_order_relaxed)) {
                c_armour_other_prog.fetch_add(1, std::memory_order_relaxed);
                const GLuint p = g_current_program.load(std::memory_order_relaxed);
                if (p && p < kMaxProgId) {
                    unsigned char e = 0;
                    if (g_prog_armour_seen[p].compare_exchange_strong(
                            e, 1, std::memory_order_relaxed)) {
                        const int s = g_prog_armour_n.fetch_add(1, std::memory_order_relaxed);
                        if (s < 8) g_prog_armour[s].store(p, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    if (k == DrawKind::Other && !held_item) return;

    c_entity_draws.fetch_add(1, std::memory_order_relaxed);
    if (entry_idx >= 0 && entry_idx < kEntryCount)
        c_entry_entity[entry_idx].fetch_add(1, std::memory_order_relaxed);

    {
        const GLuint t = g_bound_tex[0].load(std::memory_order_relaxed);
        if (t && t < kMaxTexId)
            g_tex_draws[t].fetch_add(1, std::memory_order_relaxed);
    }

    note_unaccounted(g_bound_tex[0].load(std::memory_order_relaxed),
                     g_rendering_equipment.load(std::memory_order_relaxed)
                     || g_rendering_owned.load(std::memory_order_relaxed));

    // Record which textures entity draws actually use. Bounded so a pathological
    // session cannot grow this without limit; 64 distinct entity textures is far
    // more than a scene has, and the point is to SEE the ids, not to keep them
    // all.
    // Diagnostics only, and each insert takes the lock — so it is gated. This
    // ran on every entity draw and was one of the two remaining locks in the
    // hot path.
    if (g_diag.load(std::memory_order_relaxed)) {
        if (const GLuint t0 = g_bound_tex[0].load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_entity_draw_textures.size() < 64) g_entity_draw_textures.insert(t0);
            if (g_frame_entity_tex.size() < 256) g_frame_entity_tex.insert(t0);
        }
    }

    if (k == DrawKind::Player) {
        c_player_draws.fetch_add(1, std::memory_order_relaxed);

        // ── DUMP THE SHADER OF EVERY NEW ARMOUR-RESCUE PROGRAM ─────────────
        {
            const GLuint dp = g_current_program.load(std::memory_order_relaxed);
            const GLuint dt = g_bound_tex[0].load(std::memory_order_relaxed);
            const bool armour_here =
                  dt < kMaxTexId
               && g_tex_is_armour[dt].load(std::memory_order_relaxed);
            const bool rescue_here =
                  dp && dp < kMaxProgId
               && !g_prog_is_entity[dp].load(std::memory_order_relaxed);
            if (armour_here && rescue_here
                && q_GetAttachedShaders && q_GetShaderiv && q_GetShaderSource
                && g_source_dumps_left.load(std::memory_order_relaxed) > 0) {
                unsigned char e = 0;
                if (g_prog_source_dumped[dp].compare_exchange_strong(
                        e, 1, std::memory_order_relaxed)) {
                    g_source_dumps_left.fetch_sub(1, std::memory_order_relaxed);

                    GLuint shaders[8] = {0};
                    GLsizei n = 0;
                    q_GetAttachedShaders(dp, 8, &n, shaders);
                    CHAMS_LOG("shaderdump: program %u has %d shader(s) attached; "
                              "armour tex %u was bound. Dumping source...",
                              (unsigned)dp, (int)n, (unsigned)dt);
                    for (GLsizei i = 0; i < n; ++i) {
                        GLint len = 0;
                        q_GetShaderiv(shaders[i], kShaderSourceLength, &len);
                        if (len <= 0) continue;
                        if (len > (1 << 14)) len = (1 << 14);   // 16 KB cap
                        std::string src((size_t)len, '\0');
                        GLsizei got = 0;
                        q_GetShaderSource(shaders[i], len, &got, src.data());
                        if (got <= 0) continue;
                        src.resize((size_t)got);

                        // Log in ~1200-byte chunks so nothing gets truncated
                        // by the desklog line-length cap.
                        const size_t chunk = 1200;
                        size_t off = 0, part = 0;
                        while (off < src.size()) {
                            const size_t take = std::min(chunk, src.size() - off);
                            CHAMS_LOG("shaderdump[prog=%u shader=%u part=%zu]: %.*s",
                                      (unsigned)dp, (unsigned)shaders[i], part,
                                      (int)take, src.data() + off);
                            off  += take;
                            part += 1;
                        }
                        CHAMS_LOG("shaderdump[prog=%u shader=%u end]: %zu bytes total",
                                  (unsigned)dp, (unsigned)shaders[i], src.size());
                    }
                }
            }
        }

        if (q_GetIntegervFbo) {
            GLint fbo = 0;
            q_GetIntegervFbo(kDrawFramebufferBinding, &fbo);
            const bool is_armour_tex =
                g_bound_tex[0].load(std::memory_order_relaxed) < kMaxTexId
                && g_tex_is_armour[g_bound_tex[0].load(std::memory_order_relaxed)]
                       .load(std::memory_order_relaxed);
            if (is_armour_tex) {
                (fbo == 0 ? c_armour_fbo0 : c_armour_fboN)
                    .fetch_add(1, std::memory_order_relaxed);
                if (fbo != 0)
                    g_last_armour_fbo.store((std::uint32_t)fbo,
                                            std::memory_order_relaxed);
            } else {
                (fbo == 0 ? c_player_fbo0 : c_player_fboN)
                    .fetch_add(1, std::memory_order_relaxed);
            }
        }
        (g_self_drawn_this_frame ? c_players_after_self : c_players_before_self)
            .fetch_add(1, std::memory_order_relaxed);
        {
            const GLuint at = g_bound_tex[0].load(std::memory_order_relaxed);
            if (at < kMaxTexId && g_tex_is_armour[at].load(std::memory_order_relaxed))
                c_armour_draws.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_diag.load(std::memory_order_relaxed)) {
            if (const GLuint pt = g_bound_tex[0].load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_frame_player_tex.insert(pt);
            }
        }
        // Rate-limited, but logf_site still takes the channel mutex on every
        // call to decide that — at ~70 player draws per frame that is thousands
        // of lock acquisitions a second for one line every two seconds.
        if (g_diag.load(std::memory_order_relaxed)) {
            CHAMS_RL(dbg::kSlotGlState, 2000,
                     "gl: PLAYER draw prog=%u tex0=%u depthFunc=0x%04X depthMask=%d",
                     g_current_program.load(std::memory_order_relaxed),
                     g_bound_tex[0].load(std::memory_order_relaxed),
                     static_cast<unsigned>(g_depth_func.load(std::memory_order_relaxed)),
                     static_cast<int>(g_depth_mask.load(std::memory_order_relaxed)));
        }
    }

    if (k != DrawKind::Player && !held_item) return;
    // held_item wins over the size filter even when classify() said Player:
    // that is exactly the case the threshold was deleting.

    // A held item skips the minimum-geometry test below. That test rejects a
    // skull model wearing someone's skin, which only ever arrives by TEXTURE
    // identity -- a route the item did not take. A sword is legitimately a
    // couple of quads, so measuring it against a player model would discard
    // exactly what this path exists to keep.
    if (!held_item) {

    // ── PLACED PLAYER HEADS ──────────────────────────────────────────────────
    //
    // A player head sitting on the ground is a BLOCK ENTITY, but it renders
    // through the same entity shader and binds the very same skin texture as
    // the player it belongs to. Texture identity therefore cannot tell them
    // apart -- for a head of someone currently online it is literally the same
    // GL texture object -- and that is why heads were being drawn through
    // walls alongside real players.
    //
    // Geometry can tell them apart. Minecraft's player model is twelve cuboids
    // (head, body, two arms, two legs, and the six dilated overlay parts); a
    // skull model is one cuboid plus its hat overlay. That is 432 indices
    // against 72 -- not a close call.
    //
    // The threshold sits at 100 rather than midway, because being wrong in the
    // direction of "replayed a head" is a cosmetic bug while being wrong in the
    // direction of "rejected a player" is the whole feature failing silently.
    // The counters below exist so the log shows which way it erred instead of
    // leaving it to be guessed at.
    // Armour is never a disguised skull, so the size test does not apply to it.
    const GLuint t0 = g_bound_tex[0].load(std::memory_order_relaxed);
    const bool is_armour = t0 < kMaxTexId
                        && g_tex_is_armour[t0].load(std::memory_order_relaxed) != 0;

    if (g_size_filter.load(std::memory_order_relaxed)
        && !is_armour && count >= 0 && count < kMinPlayerIndices) {
        c_reject_small.fetch_add(1, std::memory_order_relaxed);
        int prev = c_max_rejected.load(std::memory_order_relaxed);
        while (count > prev
               && !c_max_rejected.compare_exchange_weak(prev, count,
                                                        std::memory_order_relaxed)) {}
        return;
    }
    if (count >= 0) {
        int prev = c_min_kept.load(std::memory_order_relaxed);
        while (count < prev
               && !c_min_kept.compare_exchange_weak(prev, count,
                                                    std::memory_order_relaxed)) {}
    }
    }   // end of the not-a-held-item branch

    if (!g_replay.load(std::memory_order_relaxed)) return;
    if (!o_DepthFunc || !o_DepthMask) return;   // no shadow, no safe restore

    // Optionally skip BLENDED draws.
    //
    // OFF BY DEFAULT, and the reason is worth recording. The idea was that
    // replaying a blended draw composites the entity twice and smears the
    // translucent layers. The premise is true; the TEST is not. Minecraft keeps
    // GL_BLEND enabled as a general frame state, not only for translucent
    // layers — a real session showed blend=1 essentially continuously — so
    // gating on it threw away the player's opaque body and the chams vanished
    // entirely. That was the whole cause of "players stopped rendering".
    //
    // Distinguishing a genuinely translucent layer needs more than the blend
    // enable bit, so until there is a real discriminator this stays a blunt
    // instrument the user can reach for, not a default.
    if (g_skip_blended.load(std::memory_order_relaxed) &&
        g_blend_on.load(std::memory_order_relaxed)) {
        c_skipped_blend.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // REFUSE TO REPLAY ON A DEFAULTED SHADOW.
    //
    // The restore writes the shadow back into the driver. If the trackers have
    // never observed a call, the shadow still holds this file's hardcoded
    // initial values, and "restoring" them writes state the game never set —
    // which is destructive, not merely inaccurate. Silence here is the correct
    // behaviour: the log says why, and the game renders untouched.
    // SEED THE SHADOW FROM THE DRIVER RATHER THAN WAITING FOR A SETTER.
    //
    // We are inside a draw detour, so the context is current and a glGet is
    // legal and cheap here. Once seeded the hooks keep the shadow live in the
    // ordinary way; this only closes the gap at the start, which on a caching
    // layer can otherwise last the entire session.
    if (!g_shadow_seeded.load(std::memory_order_acquire)
        && q_GetIntegerv && q_GetBooleanv && q_GetFloatv) {
        GLint     f = 0;
        GLboolean m = GL_TRUE;
        GLfloat   r[2] = { 0.0f, 1.0f };
        q_GetIntegerv(kDepthFuncQ, &f);
        q_GetBooleanv(kDepthWriteMaskQ, &m);
        q_GetFloatv(kDepthRangeQ, r);
        if (f != 0) {
            // Only where the game has not already told us itself. An observed
            // call is still the better source; this fills in the rest.
            if (c_depthfunc_calls.load(std::memory_order_relaxed) == 0)
                g_depth_func.store(static_cast<GLenum>(f), std::memory_order_relaxed);
            if (c_depthmask_calls.load(std::memory_order_relaxed) == 0)
                g_depth_mask.store(m, std::memory_order_relaxed);
            if (c_depthrange_calls.load(std::memory_order_relaxed) == 0) {
                g_depth_near.store(r[0], std::memory_order_relaxed);
                g_depth_far.store(r[1], std::memory_order_relaxed);
            }
            g_shadow_seeded.store(true, std::memory_order_release);
            CHAMS_LOG("gl: depth shadow SEEDED from the driver: func=0x%04X mask=%d "
                      "range=[%.4f %.4f]. The game had called glDepthFunc %llu times "
                      "and glDepthRange %llu times -- Blaze3D caches those in Java "
                      "and only calls through on a CHANGE, so waiting to observe one "
                      "held the replay off for the whole session.",
                      (unsigned)f, (int)m, r[0], r[1],
                      c_depthfunc_calls.load(), c_depthrange_calls.load());
        }
    }

    if (!g_shadow_seeded.load(std::memory_order_acquire)
        && (c_depthfunc_calls.load(std::memory_order_relaxed) == 0
            || c_depthmask_calls.load(std::memory_order_relaxed) == 0)) {
        CHAMS_RL(dbg::kSlotCapSkip, 3000,
                 "gl: replay HELD OFF — depth shadow is at defaults and could not be "
                 "read back (depthFunc calls=%llu depthMask calls=%llu depthRange "
                 "calls=%llu, glGetIntegerv=%d). Restoring a value the game never "
                 "set would corrupt the render.",
                 c_depthfunc_calls.load(), c_depthmask_calls.load(),
                 c_depthrange_calls.load(), q_GetIntegerv != nullptr);
        return;
    }

    (void)g_depth_func.load(std::memory_order_relaxed);   // left untouched; see below
    const GLboolean saved_mask = g_depth_mask.load(std::memory_order_relaxed);

    // NOTE on glDepthRange: a real session showed depthRangeCalls = 0 with the
    // function hooked and firing for its neighbours, and resolve_gl reported no
    // separate ICD export for it — so those zeros are genuine, not blindness.
    // Minecraft simply never sets a depth range, which means the range IS the
    // GL default [0,1] and the shadow is provably correct. The earlier bail-out
    // here was over-cautious and is gone; the shadow being "never written" is
    // exactly the case where the initial value is right.

    // ── THE REPLAY OWNS ITS DEPTH ORDER ─────────────────────────────────────
    //
    //   glDepthRange(0.0, 0.01) + glDepthFunc(GL_LEQUAL) + depth writes ON
    //
    // The copy is squashed into the front 1% of the depth buffer, and INSIDE
    // that band the ordinary LEQUAL test applies with writes enabled. So the
    // model sorts against ITSELF correctly -- torso over far arm, armour over
    // body -- while sitting in front of all terrain.
    //
    // This replaces GL_GREATER with writes off, which is what produced the
    // reported flat white figure with only a fringe of armour showing. That
    // combination paints every fragment sitting behind ANYTHING, and a player's
    // own geometry qualifies: the far arm is behind the torso, the body is
    // behind its own armour. With no depth of its own the copy had no way to
    // sort, so each part painted over the last and the result was a silhouette
    // with the shading flattened out of it.
    //
    // Why the band sorts correctly ACROSS players too: the mapping from window
    // depth into [0, 0.01] is monotonic, so a nearer player still wins over a
    // farther one. It is the real ordering, only compressed, and 24-bit depth
    // leaves ~167,000 distinct values inside the band -- precision is not the
    // constraint here.
    //
    // THE COST, stated plainly because it is real and it is visible:
    // depth writes mean the band's ~0.005 stays in the buffer, so anything
    // drawn AFTERWARDS at those pixels that is not itself replayed fails LEQUAL
    // and is punched out. Held items, particles and nametags that come after a
    // player's own geometry can disappear against their silhouette. That is the
    // price of correct internal sorting, and it is why the replay set has to
    // cover everything that belongs to the player -- see the armour textures in
    // chams_jni.cpp.
    const double saved_near = g_depth_near.load(std::memory_order_relaxed);
    const double saved_far  = g_depth_far.load(std::memory_order_relaxed);

    // BAND TOP = 0.5, not 0.01. The number is the whole fix for the reported
    // "armour full of holes with the skin showing through".
    //
    // Armour sits about one model unit (1/16 of a block) proud of the body, and
    // the depth buffer is brutally non-linear, so that gap is already tiny
    // before anything compresses it. Squashing the replay into [0, 0.01]
    // divided it by another hundred, and at a 24-bit depth buffer's resolution
    // (one quantum = 2^-24) that put the two surfaces on the SAME stored value:
    //
    //     distance   body-vs-armour gap   in [0, 0.01]   in [0, 0.5]
    //         5 m         1.27e-04          21.2 q        1062 q
    //        20 m         7.84e-06           1.3 q          66 q
    //        50 m         1.25e-06           0.2 q          10 q
    //       100 m         3.13e-07           0.05 q          2.6 q
    //
    // At or below one quantum the armour and the body are indistinguishable to
    // the depth test and which one survives is decided by rounding -- per
    // pixel, so it comes out as holes. Past ~20 m the old band was ALWAYS below
    // one quantum, which matches the report that only some players were
    // affected: the near ones were fine.
    //
    // The cost of widening is the band's grip on terrain, and it is negligible.
    // A replay pinned at the top of the band is only beaten by geometry nearer
    // than 0.05/(1-top) blocks: 0.051 at 0.01, 0.100 at 0.5. Since the camera's
    // near plane is 0.05 blocks, losing to something within 0.1 blocks means
    // losing to a block that is inside your own head. Fifty times the precision
    // for that is not a close decision.
    // PASS 1 -- the chams itself.
    // THE DEPTH FUNCTION IS LEFT EXACTLY AS THE GAME SET IT.
    //
    // It used to be forced to GL_LEQUAL here, and that is what turned an
    // enchanted item into a glowing square.
    //
    // Minecraft draws the enchantment glint as a SECOND pass over the item's
    // own geometry, masked by glDepthFunc(GL_EQUAL): the glint paints only
    // where the depth matches exactly what the item's alpha-tested draw left
    // behind, so the sprite's transparent texels -- which wrote no depth -- get
    // no glint. That equality test IS the silhouette. An item model is a flat
    // quad whose shape comes entirely from texture alpha, so replacing EQUAL
    // with LEQUAL let the glint through across the whole quad: the purple
    // rectangle with a sword faintly inside it.
    //
    // Forcing it bought nothing either. The shadow reads GL_LEQUAL for ordinary
    // geometry anyway (a live session logs func=0x0203), so the band keeps the
    // cross-draw sorting armour depends on, and the one layer that deliberately
    // asks for something else now gets to keep it.
    //
    // Depth WRITES stay forced on: the glint's EQUAL test has nothing to match
    // against unless the item's own replayed draw put a value in the band.
    const GLuint rprog = g_current_program.load(std::memory_order_relaxed);
    const bool fragdepth = rprog < kMaxProgId
                        && g_prog_fragdepth[rprog].load(std::memory_order_relaxed) != 0;

    if (fragdepth) {
        // The band would do nothing here. Defeat the test instead.
        c_replay_fragdepth.fetch_add(1, std::memory_order_relaxed);
        const GLenum saved_func = g_depth_func.load(std::memory_order_relaxed);
        o_DepthFunc(GL_ALWAYS);
        o_DepthMask(GL_FALSE);
        redraw();
        o_DepthMask(saved_mask);
        o_DepthFunc(saved_func);
        c_replays.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (o_DepthRange) o_DepthRange(0.0, 0.5);
    o_DepthMask(GL_TRUE);
    redraw();

    // NO DEPTH-RESTORE PASS. It was tried, and it broke the armour.
    //
    // The idea was to redraw with colour off and the game's own depth range,
    // stamping the player's real depth over the band value so that anything
    // drawn later -- a held sword above all -- would not be deleted by our
    // footprint. It does undo the punch-out. It also undoes the ONE thing the
    // band exists for.
    //
    // Cross-draw ordering is what makes armour sit on top of the body, and it
    // works because both draws leave their value INSIDE the band, where they
    // can be compared with each other. Overwriting that with the real depth
    // means the armour's draw arrives with nothing to test against: it passes
    // unconditionally, and which of body or armour survives goes back to being
    // decided by whoever drew last. That is the flat, holed armour again.
    //
    // The punch-out is therefore not a bug to be patched around -- it is the
    // direct price of the depth writes that make the model sort correctly, and
    // anything that refunds it takes the sorting with it.

    o_DepthMask(saved_mask);
    if (o_DepthRange) o_DepthRange(saved_near, saved_far);

    c_replays.fetch_add(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw detours. Every one is the same shape: guard, observe, call through, and —
// for the first-person item only — add the rim pass on top afterwards.
#define CHAMS_DRAW_BODY(entry, idx, cnt, call)                        \
    g_in_detour.fetch_add(1, std::memory_order_acquire);              \
    ++t_in_detour;                                                    \
    struct leave_t { ~leave_t() { --t_in_detour;                      \
        g_in_detour.fetch_sub(1, std::memory_order_release); } } _lv; \
    t_item_outline = false;                                          \
    if (!passthrough()) {                                            \
        reentry_guard _rg;                                           \
        if (_rg.ok)                                                  \
            observe_and_maybe_replay(entry, idx, (cnt), [&]{ call; });\
    } else {                                                         \
        c_suppressed.fetch_add(1, std::memory_order_relaxed);        \
    }                                                                \
    if (t_item_outline) { t_item_outline = false;                     \
        ::vanta::modules::itemshader::run_capture([&]{ call; }); }    \
    call;

void APIENTRY hk_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* idx)
{
    CHAMS_DRAW_BODY("glDrawElements", kEDrawElements, count, o_DrawElements(mode, count, type, idx))
}

void APIENTRY hk_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    CHAMS_DRAW_BODY("glDrawArrays", kEDrawArrays, count, o_DrawArrays(mode, first, count))
}

void APIENTRY hk_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                        const void* idx, GLint base)
{
    CHAMS_DRAW_BODY("glDrawElementsBaseVertex", kEDrawElementsBaseVertex, count,
                    o_DrawElementsBaseVertex(mode, count, type, idx, base))
}

void APIENTRY hk_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                       const void* idx, GLsizei prim)
{
    CHAMS_DRAW_BODY("glDrawElementsInstanced", kEDrawElementsInstanced, count,
                    o_DrawElementsInstanced(mode, count, type, idx, prim))
}

void APIENTRY hk_DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                                 const void* idx, GLsizei prim, GLint base)
{
    CHAMS_DRAW_BODY("glDrawElementsInstancedBaseVertex", kEDrawElementsInstancedBaseVertex, count,
                    o_DrawElementsInstancedBaseVertex(mode, count, type, idx, prim, base))
}

void APIENTRY hk_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei prim)
{
    CHAMS_DRAW_BODY("glDrawArraysInstanced", kEDrawArraysInstanced, count,
                    o_DrawArraysInstanced(mode, first, count, prim))
}

void APIENTRY hk_MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                             const void* const* idx, GLsizei drawcount,
                                             const GLint* base)
{
    // Sodium's terrain path is the only known user of this on the target
    // versions. Tracked so the log can PROVE that terrain does not come through
    // the entity classifier, rather than assuming it.
    CHAMS_DRAW_BODY("glMultiDrawElementsBaseVertex", kEMultiDrawElementsBaseVertex, -1,
                    o_MultiDrawElementsBaseVertex(mode, count, type, idx, drawcount, base))
}

#undef CHAMS_DRAW_BODY

// ─────────────────────────────────────────────────────────────────────────────
// State trackers. These never change behaviour — they only record.

void APIENTRY hk_ShaderSource(GLuint shader, GLsizei count,
                              const GLchar* const* strings, const GLint* lengths)
{
    if (!g_suppressed.load(std::memory_order_relaxed) && strings && count > 0) {
        reentry_guard rg;
        if (rg.ok) {
            // Concatenate honouring `lengths`: LWJGL passes a non-null length
            // array, so the strings are NOT necessarily NUL-terminated and
            // strlen would read past the buffer.
            std::string src;
            for (GLsizei i = 0; i < count; ++i) {
                if (!strings[i]) continue;
                if (lengths && lengths[i] >= 0) src.append(strings[i], static_cast<size_t>(lengths[i]));
                else                            src.append(strings[i]);
            }
            c_shaders_seen.fetch_add(1, std::memory_order_relaxed);

            // The entity shader is the only one of Minecraft's 93 core shaders
            // carrying this identifier. One string, no mappings, no version
            // dependence.
            if (src.find("overlayColor") != std::string::npos) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_entity_shaders.insert(shader);
                c_entity_shaders.fetch_add(1, std::memory_order_relaxed);
                CHAMS_LOG("gl: entity shader source matched (shader=%u, %zu bytes)",
                          shader, src.size());
            }
        }
    }
    o_ShaderSource(shader, count, strings, lengths);
}

void APIENTRY hk_AttachShader(GLuint program, GLuint shader)
{
    if (!g_suppressed.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_program_shaders[program].push_back(shader);
    }
    o_AttachShader(program, shader);
}

void APIENTRY hk_LinkProgram(GLuint program)
{
    if (!g_suppressed.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mx);
        c_programs_linked.fetch_add(1, std::memory_order_relaxed);
        auto it = g_program_shaders.find(program);
        if (it != g_program_shaders.end()) {
            for (GLuint s : it->second) {
                if (g_entity_shaders.count(s)) {
                    g_entity_programs.insert(program);
                    flag_entity_program(program);
                    c_entity_programs.fetch_add(1, std::memory_order_relaxed);
                    CHAMS_LOG("gl: program %u classified ENTITY (via shader %u)", program, s);
                    break;
                }
            }
        }
    }
    o_LinkProgram(program);
}

void APIENTRY hk_UseProgram(GLuint program)
{
    if (!g_suppressed.load(std::memory_order_relaxed)) {
        g_current_program.store(program, std::memory_order_relaxed);
        // Record it for the deferred classifier. No GL is issued here — this
        // runs inside a detour, and a GL error here is an in-driver abort.
        // Fast path: a program already flagged as entity, or already
        // classified, needs no lock. Only genuinely new ids reach the mutex,
        // which after the first seconds is never.
        if (program != 0 && program < kMaxProgId
            && !g_prog_is_entity[program].load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_classified_programs.count(program))
                g_seen_programs.insert(program);
        }
    }
    o_UseProgram(program);
}

void APIENTRY hk_ActiveTexture(GLenum unit)
{
    if (!g_suppressed.load(std::memory_order_relaxed)) {
        c_active_calls.fetch_add(1, std::memory_order_relaxed);
        const int u = static_cast<int>(unit) - static_cast<int>(kTexture0);
        if (u >= 0 && u < kMaxUnits) g_active_unit.store(u, std::memory_order_relaxed);
    }
    o_ActiveTexture(unit);
}

// Shared body: the same tracking whether the call arrived through opengl32's
// export or the ICD's. Both are hooked when they resolve to different
// addresses, because which one the game actually calls for a GL 1.1 entry point
// is not something to assume — the first session's permanent tex0 = 0 is
// exactly what "assumed wrong" looks like.
inline void track_bind(GLenum target, GLuint texture)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_bind_calls.fetch_add(1, std::memory_order_relaxed);
    if (target != GL_TEXTURE_2D) return;
    const int u = g_active_unit.load(std::memory_order_relaxed);
    if (u >= 0 && u < kMaxUnits) {
        g_bound_tex[u].store(texture, std::memory_order_relaxed);
        c_bind_unit[u].fetch_add(1, std::memory_order_relaxed);
    }
}

void APIENTRY hk_BindTexture(GLenum target, GLuint texture)
{
    track_bind(target, texture);
    o_BindTexture(target, texture);
}

PFN_glBindTexture o_BindTextureIcd = nullptr;
void APIENTRY hk_BindTextureIcd(GLenum target, GLuint texture)
{
    track_bind(target, texture);
    o_BindTextureIcd(target, texture);
}

// The two the verifier flagged as missing. Without them the chams pass has no
// way to know what to restore, because reading state back with glGetIntegerv
// inside a per-draw detour is not affordable — and getting the restore wrong is
// the "world renders wrong for the rest of the session, no error anywhere"
// failure mode.
// ── GL 1.1 state: BOTH copies must be hooked ─────────────────────────────────
//
// opengl32.dll exports these AND the ICD exports its own. Which one a caller
// reaches depends on how it resolved the symbol, and they are different
// functions at different addresses. glBindTexture proved this the hard way:
// hooking only the opengl32 export produced ZERO observed calls, and adding the
// ICD twin made the counter jump to hundreds of thousands.
//
// For a tracker, missing the calls means a stale shadow. For the REPLAY, a
// stale shadow is actively destructive: it restores a value the game never set.
// Restoring glDepthRange to a guessed [0,1] clobbers the compressed range
// Minecraft uses for the first-person hand, which is how "the players stopped
// rendering" happens.
//
// So: every state function is hooked twice, and the trackers count their calls
// so the log can prove the shadow is live rather than defaulted.
inline void track_depth_func(GLenum func)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_depthfunc_calls.fetch_add(1, std::memory_order_relaxed);
    g_depth_func.store(func, std::memory_order_relaxed);
}
inline void track_depth_mask(GLboolean flag)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_depthmask_calls.fetch_add(1, std::memory_order_relaxed);
    g_depth_mask.store(flag, std::memory_order_relaxed);
}
inline void track_depth_range(GLdouble n, GLdouble f)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_depthrange_calls.fetch_add(1, std::memory_order_relaxed);
    g_depth_near.store(n, std::memory_order_relaxed);
    g_depth_far.store(f, std::memory_order_relaxed);
}
inline void track_cap(GLenum cap, bool on)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    if (cap == GL_DEPTH_TEST) g_depth_test_on.store(on, std::memory_order_relaxed);
    else if (cap == GL_BLEND) g_blend_on.store(on, std::memory_order_relaxed);
}

PFN_glDepthFunc  o_DepthFuncIcd  = nullptr;
PFN_glDepthMask  o_DepthMaskIcd  = nullptr;
PFN_glDepthRange o_DepthRangeIcd = nullptr;
PFN_glEnable     o_EnableIcd     = nullptr;
PFN_glDisable    o_DisableIcd    = nullptr;
PFN_glColorMask  o_ColorMaskIcd  = nullptr;

void APIENTRY hk_DepthFuncIcd(GLenum f)               { track_depth_func(f);      o_DepthFuncIcd(f); }
void APIENTRY hk_DepthMaskIcd(GLboolean m)            { track_depth_mask(m);      o_DepthMaskIcd(m); }
void APIENTRY hk_DepthRangeIcd(GLdouble n, GLdouble f){ track_depth_range(n, f);  o_DepthRangeIcd(n, f); }
void APIENTRY hk_EnableIcd(GLenum c)                  { track_cap(c, true);       o_EnableIcd(c); }
void APIENTRY hk_DisableIcd(GLenum c)                 { track_cap(c, false);      o_DisableIcd(c); }

inline void track_colour_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_colourmask_calls.fetch_add(1, std::memory_order_relaxed);
    g_colour_mask.store(static_cast<unsigned char>((r ? 1 : 0) | (g ? 2 : 0)
                                                 | (b ? 4 : 0) | (a ? 8 : 0)),
                        std::memory_order_relaxed);
}
void APIENTRY hk_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ track_colour_mask(r, g, b, a); o_ColorMask(r, g, b, a); }
void APIENTRY hk_ColorMaskIcd(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ track_colour_mask(r, g, b, a); o_ColorMaskIcd(r, g, b, a); }

void APIENTRY hk_DepthFunc(GLenum func)
{
    track_depth_func(func);
    o_DepthFunc(func);
}

void APIENTRY hk_DepthMask(GLboolean flag)  { track_depth_mask(flag);  o_DepthMask(flag); }
// Record textures uploaded at player-skin dimensions. Both entry points are
// covered because Minecraft has used each across the supported range.
// RETIRED, and kept as a stub so the two call sites stay honest about it.
//
// This used to flag every base-level texture uploaded at 64x64 or 64x32 as a
// player skin, and union those ids into the replay set. It was a workaround for
// a diagnosis that turned out to be WRONG: players were going missing because
// tr7zw's EntityCulling mod was skipping them, not because skin resolution was
// failing. Once the real cause was fixed the JNI route began resolving every
// player -- the log reads "player skins resolved: 30 of 30" and playerTex=30 --
// so the fallback had nothing left to contribute.
//
// What it DID still contribute was two bugs, and the log measured them:
// skinSized=39 against playerTex=30, i.e. NINE textures being replayed that no
// player owns. Vanilla armour layers (textures/models/armor/*_layer_*.png) are
// 64x32 -- exactly the "legacy skin" pattern -- and a placed player head loads
// its own 64x64 skin. Both matched, so both were drawn through walls: the
// armour bug and the floating-heads bug were one heuristic.
//
// A dimension is not an identity. The JNI route asks the game which texture
// belongs to which player and gets an answer; this guessed from a shape that
// three unrelated things happen to share.
inline void note_texture_upload(GLsizei, GLsizei, GLint) {}

void APIENTRY hk_TexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h,
                            GLint b, GLenum f, GLenum ty, const void* px)
{
    note_texture_upload(w, h, lvl);
    o_TexImage2D(t, lvl, ifmt, w, h, b, f, ty, px);
}

void APIENTRY hk_TexStorage2D(GLenum t, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h)
{
    note_texture_upload(w, h, 0);
    o_TexStorage2D(t, levels, ifmt, w, h);
}

inline void track_blend(GLenum sr, GLenum dr, GLenum sa, GLenum da)
{
    if (g_suppressed.load(std::memory_order_relaxed)) return;
    c_blendfunc_calls.fetch_add(1, std::memory_order_relaxed);
    g_blend_src_rgb.store(sr, std::memory_order_relaxed);
    g_blend_dst_rgb.store(dr, std::memory_order_relaxed);
    g_blend_src_a.store(sa, std::memory_order_relaxed);
    g_blend_dst_a.store(da, std::memory_order_relaxed);
}

void APIENTRY hk_BlendFunc(GLenum src, GLenum dst)
{
    track_blend(src, dst, src, dst);   // the 2-arg form sets both pairs
    o_BlendFunc(src, dst);
}

void APIENTRY hk_BlendFuncSep(GLenum sr, GLenum dr, GLenum sa, GLenum da)
{
    track_blend(sr, dr, sa, da);
    o_BlendFuncSep(sr, dr, sa, da);
}

void APIENTRY hk_DepthRange(GLdouble n, GLdouble f) { track_depth_range(n, f); o_DepthRange(n, f); }
void APIENTRY hk_Enable(GLenum cap)         { track_cap(cap, true);    o_Enable(cap); }
void APIENTRY hk_Disable(GLenum cap)        { track_cap(cap, false);   o_Disable(cap); }

// ─────────────────────────────────────────────────────────────────────────────
// Entry-point resolution, in LWJGL's order.
//
// wglGetProcAddress FIRST. On NVIDIA it returns an address inside nvoglv64.dll,
// which is what the game calls; GetProcAddress(opengl32) returns a DIFFERENT
// stub for the few functions it exports at all, and hooking that intercepts
// nothing. Verified on this machine with a probe that created a context and
// compared both paths.
void* resolve_gl(const char* name)
{
    if (void* p = reinterpret_cast<void*>(wglGetProcAddress(name))) {
        // wglGetProcAddress historically returns these sentinels for "no".
        const intptr_t v = reinterpret_cast<intptr_t>(p);
        if (v != 0 && v != 1 && v != 2 && v != 3 && v != -1) return p;
    }
    if (HMODULE gl = GetModuleHandleA("opengl32.dll"))
        return reinterpret_cast<void*>(GetProcAddress(gl, name));
    return nullptr;
}

// Fixed-function state entry points live ONLY in opengl32 — wglGetProcAddress
// returns NULL for them. Asking in the wrong order silently yields nothing.
void* resolve_gl11(const char* name)
{
    if (HMODULE gl = GetModuleHandleA("opengl32.dll"))
        if (void* p = reinterpret_cast<void*>(GetProcAddress(gl, name)))
            return p;
    return reinterpret_cast<void*>(wglGetProcAddress(name));
}

struct hook_spec {
    const char* name;
    void*       detour;
    void**      original;
    bool        gl11;        // resolve from opengl32 rather than the ICD
    bool        required;
};

// The address hook_one actually patched, per function name. The ICD-twin check
// must compare against THIS, not against the `original` pointer: MinHook sets
// that to a trampoline it allocated, which never equals the target, so
// comparing against it made every twin look distinct and try to hook the same
// address twice.
std::unordered_map<std::string, void*> g_hooked_target;

// EVERY address this module patched, primaries and ICD twins alike.
//
// uninstall() used to call MH_DisableHook(MH_ALL_HOOKS) / MH_RemoveHook(
// MH_ALL_HOOKS). Those constants are PROCESS-GLOBAL: they tear down every hook
// MinHook holds, which in this process also means gl_hook.cc's wglSwapBuffers
// detour -- the one driving the entire overlay, and the one whose stack frame
// uninstall() is running on -- plus cursor_guard.cc's. Removing hooks we do not
// own, from inside one of them, is how a clean unload turns into a crash.
std::vector<void*> g_patched;

bool hook_one(const hook_spec& s)
{
    void* target = s.gl11 ? resolve_gl11(s.name) : resolve_gl(s.name);
    if (!target) {
        ::vanta::desklog::jni_lookup(dbg::ch(), "glproc", s.gl11 ? "opengl32" : "icd",
                                     s.name, "<unresolved>", false);
        return !s.required;
    }

    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(target), &owner);
    char ownerName[MAX_PATH] = "?";
    if (owner) GetModuleFileNameA(owner, ownerName, MAX_PATH);

    const MH_STATUS st = MH_CreateHook(target, s.detour, s.original);
    const bool ok = (st == MH_OK) && (MH_EnableHook(target) == MH_OK);
    if (ok) { g_hooked_target[s.name] = target; g_patched.push_back(target); }

    CHAMS_LOG("gl: %-36s target=%p owner=%s  %s",
              s.name, target, ownerName, ok ? "HOOKED" : "FAILED");
    ::vanta::desklog::jni_lookup(dbg::ch(), "glproc", s.gl11 ? "opengl32" : "icd",
                                 s.name, ownerName, ok);
    return ok || !s.required;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
bool install()
{
    if (g_installed.load(std::memory_order_acquire)) return true;

    // A current context is mandatory: wglGetProcAddress returns NULL without
    // one, which is why this is driven from inside the swap detour rather than
    // from Bootstrap.
    if (!wglGetCurrentContext()) {
        CHAMS_RL(dbg::kSlotGlState, 2000, "gl: install deferred — no current GL context");
        return false;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        CHAMS_LOG("gl: MH_Initialize failed (%d)", static_cast<int>(init));
        return false;
    }

    ::vanta::desklog::section(dbg::ch(), "GL hook installation");
    CHAMS_LOG("resolution order: wglGetProcAddress first (the ICD address LWJGL "
              "caches), GetProcAddress(opengl32) only as a fallback.");

    const hook_spec specs[] = {
        // Draw entry points. Only glDrawElements exists on 1.21.0-1.21.4; the
        // rest appear on 1.21.5+. None is individually required, but at least
        // one must land — checked below.
        { "glDrawElements",                    &hk_DrawElements,                    reinterpret_cast<void**>(&o_DrawElements),                    false, false },
        { "glDrawArrays",                      &hk_DrawArrays,                      reinterpret_cast<void**>(&o_DrawArrays),                      false, false },
        { "glDrawElementsBaseVertex",          &hk_DrawElementsBaseVertex,          reinterpret_cast<void**>(&o_DrawElementsBaseVertex),          false, false },
        { "glDrawElementsInstanced",           &hk_DrawElementsInstanced,           reinterpret_cast<void**>(&o_DrawElementsInstanced),           false, false },
        { "glDrawElementsInstancedBaseVertex", &hk_DrawElementsInstancedBaseVertex, reinterpret_cast<void**>(&o_DrawElementsInstancedBaseVertex), false, false },
        { "glDrawArraysInstanced",             &hk_DrawArraysInstanced,             reinterpret_cast<void**>(&o_DrawArraysInstanced),             false, false },
        { "glMultiDrawElementsBaseVertex",     &hk_MultiDrawElementsBaseVertex,     reinterpret_cast<void**>(&o_MultiDrawElementsBaseVertex),     false, false },

        // Classifier inputs. Without ShaderSource/LinkProgram/UseProgram there
        // is no classification at all, so those three are required.
        { "glShaderSource",   &hk_ShaderSource,   reinterpret_cast<void**>(&o_ShaderSource),   false, true  },
        { "glAttachShader",   &hk_AttachShader,   reinterpret_cast<void**>(&o_AttachShader),   false, true  },
        { "glLinkProgram",    &hk_LinkProgram,    reinterpret_cast<void**>(&o_LinkProgram),    false, true  },
        { "glUseProgram",     &hk_UseProgram,     reinterpret_cast<void**>(&o_UseProgram),     false, true  },
        { "glActiveTexture",  &hk_ActiveTexture,  reinterpret_cast<void**>(&o_ActiveTexture),  false, false },

        // GL 1.1 — opengl32 exports, NULL from wglGetProcAddress.
        { "glBindTexture", &hk_BindTexture, reinterpret_cast<void**>(&o_BindTexture), true, false },
        { "glDepthFunc",   &hk_DepthFunc,   reinterpret_cast<void**>(&o_DepthFunc),   true, false },
        { "glDepthMask",   &hk_DepthMask,   reinterpret_cast<void**>(&o_DepthMask),   true, false },
        { "glDepthRange",  &hk_DepthRange,  reinterpret_cast<void**>(&o_DepthRange),  true, false },
        { "glTexImage2D",  &hk_TexImage2D,  reinterpret_cast<void**>(&o_TexImage2D),  true, false },
        { "glTexStorage2D",&hk_TexStorage2D,reinterpret_cast<void**>(&o_TexStorage2D),false,false },
        { "glBlendFunc",   &hk_BlendFunc,   reinterpret_cast<void**>(&o_BlendFunc),   true, false },
        // The one Blaze3D actually calls.
        { "glBlendFuncSeparate", &hk_BlendFuncSep, reinterpret_cast<void**>(&o_BlendFuncSep), false, false },
        { "glEnable",      &hk_Enable,      reinterpret_cast<void**>(&o_Enable),      true, false },
        { "glDisable",     &hk_Disable,     reinterpret_cast<void**>(&o_Disable),     true, false },
        // Required by the depth-restore pass, which redraws with colour off.
        { "glColorMask",   &hk_ColorMask,   reinterpret_cast<void**>(&o_ColorMask),   true, false },
    };

    bool all_required = true;
    for (const hook_spec& s : specs)
        if (!hook_one(s)) all_required = false;

    // ── ICD twins of every GL 1.1 state function ─────────────────────────────
    //
    // opengl32.dll exports these AND the ICD exports its own, at different
    // addresses. glBindTexture proved which one matters: hooking only the
    // opengl32 export observed ZERO calls, and adding the ICD twin took the
    // counter to hundreds of thousands.
    //
    // For glDepthFunc / glDepthMask / glDepthRange this is not a diagnostics
    // nicety — it is a correctness requirement. The replay RESTORES those values
    // from the shadow, and a shadow that never saw a call restores the
    // hardcoded default instead. Putting glDepthRange back to [0,1] clobbers the
    // compressed range Minecraft uses for the first-person hand, which is
    // exactly how "the players stopped rendering" happened.
    struct twin { const char* name; void* detour; void** orig; };
    const twin twins[] = {
        { "glBindTexture", &hk_BindTextureIcd, reinterpret_cast<void**>(&o_BindTextureIcd) },
        { "glDepthFunc",   &hk_DepthFuncIcd,   reinterpret_cast<void**>(&o_DepthFuncIcd)   },
        { "glDepthMask",   &hk_DepthMaskIcd,   reinterpret_cast<void**>(&o_DepthMaskIcd)   },
        { "glDepthRange",  &hk_DepthRangeIcd,  reinterpret_cast<void**>(&o_DepthRangeIcd)  },
        { "glEnable",      &hk_EnableIcd,      reinterpret_cast<void**>(&o_EnableIcd)      },
        { "glDisable",     &hk_DisableIcd,     reinterpret_cast<void**>(&o_DisableIcd)     },
        { "glColorMask",   &hk_ColorMaskIcd,   reinterpret_cast<void**>(&o_ColorMaskIcd)   },
    };
    for (const twin& t : twins) {
        void* icd = resolve_gl(t.name);
        auto it = g_hooked_target.find(t.name);
        void* primary = (it == g_hooked_target.end()) ? nullptr : it->second;
        if (!icd) { CHAMS_LOG("gl: %-14s ICD copy: not exported", t.name); continue; }
        if (icd == primary) {
            CHAMS_LOG("gl: %-14s ICD address == opengl32 address, one hook covers both", t.name);
            continue;
        }
        const bool ok = MH_CreateHook(icd, t.detour, t.orig) == MH_OK
                     && MH_EnableHook(icd) == MH_OK;
        if (ok) g_patched.push_back(icd);
        CHAMS_LOG("gl: %-14s (ICD copy)  target=%p  %s",
                  t.name, icd, ok ? "HOOKED" : "FAILED");
    }

    // Query entry points for the deferred classifier. Resolved, NOT hooked —
    // these are what let a program be classified long after its shaders were
    // compiled, which is the normal case when injecting into a running game.
    q_GetAttachedShaders = reinterpret_cast<PFN_glGetAttachedShaders>(resolve_gl("glGetAttachedShaders"));
    q_GetShaderSource    = reinterpret_cast<PFN_glGetShaderSource>(resolve_gl("glGetShaderSource"));
    q_GetShaderiv        = reinterpret_cast<PFN_glGetShaderiv>(resolve_gl("glGetShaderiv"));
    q_IsProgram          = reinterpret_cast<PFN_glIsProgram>(resolve_gl("glIsProgram"));
    q_BlendColor         = reinterpret_cast<PFN_glBlendColor>(resolve_gl("glBlendColor"));
    q_GenTextures        = reinterpret_cast<PFN_glGenTextures>(resolve_gl11("glGenTextures"));
    // GL 1.1, so opengl32 rather than the ICD -- same rule as glGenTextures.
    q_GetIntegerv        = reinterpret_cast<PFN_glGetIntegerv>(resolve_gl11("glGetIntegerv"));
    q_IsEnabled          = reinterpret_cast<PFN_glIsEnabled>(resolve_gl11("glIsEnabled"));
    q_GetBooleanv        = reinterpret_cast<PFN_glGetBooleanv>(resolve_gl11("glGetBooleanv"));
    q_GetFloatv          = reinterpret_cast<PFN_glGetFloatv>(resolve_gl11("glGetFloatv"));
    // glGetIntegerv is already resolved above as q_GetIntegerv; reuse it.
    q_GetIntegervFbo     = q_GetIntegerv;
    CHAMS_LOG("gl: deferred-classifier queries: getAttachedShaders=%d getShaderSource=%d "
              "getShaderiv=%d isProgram=%d",
              q_GetAttachedShaders != nullptr, q_GetShaderSource != nullptr,
              q_GetShaderiv != nullptr, q_IsProgram != nullptr);

    const bool any_draw = o_DrawElements || o_DrawArrays || o_DrawElementsBaseVertex
                       || o_DrawElementsInstanced || o_DrawElementsInstancedBaseVertex
                       || o_DrawArraysInstanced;

    if (!all_required || !any_draw) {
        CHAMS_LOG("gl: install INCOMPLETE (required=%d anyDraw=%d) — staying inert",
                  (int)all_required, (int)any_draw);
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    CHAMS_LOG("gl: install complete. STAGE 1 — instrumentation only, no GL state "
              "is modified and every original is called unchanged.");
    return true;
}

bool uninstall()
{
    if (!g_installed.load(std::memory_order_acquire) && g_patched.empty()) return true;
    g_active.store(false, std::memory_order_release);

    // ONLY the addresses this module patched. See g_patched.
    for (void* t : g_patched) {
        const auto result = MH_DisableHook(t);
        if (result != MH_OK && result != MH_ERROR_DISABLED) return false;
    }

    // Drain: MH_RemoveHook frees trampolines, and a thread still inside a
    // detour would return into unmapped memory.
    for (int i = 0; i < 2000 && g_in_detour.load(std::memory_order_acquire) > 0; ++i)
        Sleep(1);

    if (g_in_detour.load(std::memory_order_acquire) > 0) return false;
    for (void* t : g_patched) {
        const auto result = MH_RemoveHook(t);
        if (result != MH_OK && result != MH_ERROR_NOT_CREATED) return false;
    }
    CHAMS_LOG("gl: removed %zu hook(s) -- ours only; other subsystems untouched",
              g_patched.size());
    g_patched.clear();
    g_hooked_target.clear();

    o_DrawElements = nullptr; o_DrawArrays = nullptr;
    o_DrawElementsBaseVertex = nullptr; o_DrawElementsInstanced = nullptr;
    o_DrawElementsInstancedBaseVertex = nullptr; o_DrawArraysInstanced = nullptr;
    o_MultiDrawElementsBaseVertex = nullptr;
    o_ShaderSource = nullptr; o_AttachShader = nullptr; o_LinkProgram = nullptr;
    o_UseProgram = nullptr; o_ActiveTexture = nullptr; o_BindTexture = nullptr;
    o_DepthFunc = nullptr; o_DepthMask = nullptr;

    CHAMS_LOG("gl: uninstalled");
    g_installed.store(false, std::memory_order_release);
    return true;
}

int  in_detour()          { return g_in_detour.load(std::memory_order_acquire); }
int  in_detour_this_thread() { return t_in_detour; }
void set_size_filter(bool on) { g_size_filter.store(on, std::memory_order_relaxed); }
void set_rendering_player(bool on) { g_rendering_player.store(on, std::memory_order_relaxed); }
void set_rendering_equipment(bool on) { g_rendering_equipment.store(on, std::memory_order_relaxed); }
void set_rendering_owned_equipment(bool on) { g_rendering_owned.store(on, std::memory_order_relaxed); }
bool rendering_player() { return g_rendering_player.load(std::memory_order_relaxed); }

// The first-person item-shader controls now live in item_shader_gl.cpp; the
// Animations detour and the ItemShader module call that namespace directly.

bool installed()          { return g_installed.load(std::memory_order_acquire); }
void set_active(bool on)  { g_active.store(on, std::memory_order_release); }
bool active()             { return g_active.load(std::memory_order_acquire); }
void set_suppressed(bool on) { g_suppressed.store(on, std::memory_order_release); }
bool exchange_suppressed(bool on) { return g_suppressed.exchange(on, std::memory_order_acq_rel); }

void publish_self_texture(std::uint32_t name)
{
    g_self_tex.store(name, std::memory_order_relaxed);
}

int top_textures(std::uint32_t* ids, std::uint32_t* counts, unsigned char* flags, int cap)
{
    // A linear selection over 32768 slots, a few times a minute. Sorting would
    // be tidier and would also mean copying the table while the render thread
    // writes it; this reads each slot once and never blocks a draw.
    int n = 0;
    for (GLuint t = 1; t < kMaxTexId; ++t) {
        const std::uint32_t c = g_tex_draws[t].load(std::memory_order_relaxed);
        if (!c) continue;
        int at = n;
        if (n == cap) {
            int worst = 0;
            for (int i = 1; i < n; ++i) if (counts[i] < counts[worst]) worst = i;
            if (counts[worst] >= c) continue;
            at = worst;
        } else {
            ++n;
        }
        ids[at]    = t;
        counts[at] = c;
        flags[at]  = (unsigned char)
                     ((g_tex_is_player[t].load(std::memory_order_relaxed) ? 1 : 0)
                    | (g_tex_is_armour[t].load(std::memory_order_relaxed) ? 2 : 0)
                    | (t == g_self_tex.load(std::memory_order_relaxed)    ? 4 : 0));
    }
    // Descending, so the busiest row is the first thing read.
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (counts[j] > counts[i]) {
                std::swap(counts[i], counts[j]);
                std::swap(ids[i], ids[j]);
                std::swap(flags[i], flags[j]);
            }
    return n;
}

int unaccounted_textures(std::uint32_t* out, int cap)
{
    const int n = g_unknown_n.load(std::memory_order_relaxed);
    const int have = n < kMaxUnknown ? n : kMaxUnknown;
    const int w = have < cap ? have : cap;
    for (int i = 0; i < w; ++i)
        out[i] = g_unknown_id[i].load(std::memory_order_relaxed);
    return w;
}

int equipment_unaccounted(std::uint32_t* out, int cap, unsigned long long& known,
                          unsigned long long& unknown)
{
    known   = c_equip_known.load(std::memory_order_relaxed);
    unknown = c_equip_unknown.load(std::memory_order_relaxed);
    const int n = g_equip_n.load(std::memory_order_relaxed);
    const int have = n < kMaxUnknown ? n : kMaxUnknown;
    const int w = have < cap ? have : cap;
    for (int i = 0; i < w; ++i)
        out[i] = g_equip_id[i].load(std::memory_order_relaxed);
    return w;
}

void publish_armour_textures(const std::uint32_t* names, int count)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (GLuint n : g_flagged_armour_tex)
        if (n < kMaxTexId) g_tex_is_armour[n].store(0, std::memory_order_relaxed);
    g_flagged_armour_tex.clear();
    for (int i = 0; i < count; ++i) {
        const GLuint n = static_cast<GLuint>(names[i]);
        if (!n || n >= kMaxTexId) continue;
        g_tex_is_armour[n].store(1, std::memory_order_relaxed);
        g_flagged_armour_tex.push_back(n);
    }
}

void publish_player_textures(const std::uint32_t* names, int count)
{
    std::lock_guard<std::mutex> lk(g_mx);

    // Clear only what was flagged last time; wiping the whole 32 KB table every
    // frame would cost more than the lookup it exists to make cheap.
    for (GLuint n : g_flagged_player_tex)
        if (n < kMaxTexId) g_tex_is_player[n].store(0, std::memory_order_relaxed);
    g_flagged_player_tex.clear();

    g_player_textures.clear();
    for (int i = 0; i < count; ++i) {
        const GLuint n = static_cast<GLuint>(names[i]);
        if (!n) continue;
        g_player_textures.insert(n);
        if (n < kMaxTexId) {
            g_tex_is_player[n].store(1, std::memory_order_relaxed);
            g_flagged_player_tex.push_back(n);
        }
    }
    // NO dimension-based union any more. The JNI ids above are the whole set:
    // they are the only identities that came from the game rather than from a
    // guess. See note_texture_upload.
}

void classify_pending_programs()
{
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!q_GetAttachedShaders || !q_GetShaderSource || !q_GetShaderiv) return;
    if (!wglGetCurrentContext()) return;

    // Take a small batch out of the queue. Bounded so a pathological frame
    // cannot stall the render thread reading back hundreds of shader sources.
    std::vector<GLuint> batch;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (GLuint p : g_seen_programs) {
            if (g_classified_programs.count(p)) continue;
            batch.push_back(p);
            if (batch.size() >= 8) break;
        }
    }
    if (batch.empty()) return;

    // Suppress the trackers while we issue our own GL: these calls run on the
    // game's context and would otherwise be seen by our own detours.
    const bool prev = g_suppressed.exchange(true, std::memory_order_acq_rel);

    CHAMS_MARK("gl::classify_pending_programs -> shader source read-back");
    for (GLuint prog : batch) {
        bool is_entity = false;
        bool writes_fragdepth = false;
        bool has_normal = false;

        if (!q_IsProgram || q_IsProgram(prog) == GL_TRUE) {
            GLuint shaders[8] = {};
            GLsizei n = 0;
            q_GetAttachedShaders(prog, 8, &n, shaders);

            // Read ALL attached shaders (no early break): has_normal must see the
            // vertex shader even after the fragment shader already proved entity.
            for (GLsizei i = 0; i < n; ++i) {
                GLint len = 0;
                q_GetShaderiv(shaders[i], kShaderSourceLength, &len);
                if (len <= 0 || len > (1 << 20)) continue;

                std::string src(static_cast<size_t>(len), '\0');
                GLsizei got = 0;
                q_GetShaderSource(shaders[i], len, &got, src.data());
                if (got > 0) {
                    src.resize(static_cast<size_t>(got));
                    // The identifier that appears in Minecraft's entity shader
                    // and in none of the other core shaders.
                    if (src.find("overlayColor") != std::string::npos) is_entity = true;
                    // And the one that makes the replay's depth trick inert.
                    if (src.find("gl_FragDepth") != std::string::npos)
                        writes_fragdepth = true;
                    // Lit 3D geometry declares a Normal attribute; the 2D GUI/HUD
                    // shaders do not. This is what lets the outline catch a held
                    // block (not an entity program) yet skip the HUD.
                    if (src.find("in vec3 Normal") != std::string::npos) has_normal = true;
                }
            }
        }

        if (has_normal && prog < kMaxProgId)
            g_prog_has_normal[prog].store(1, std::memory_order_relaxed);

        if (writes_fragdepth && prog < kMaxProgId) {
            g_prog_fragdepth[prog].store(1, std::memory_order_relaxed);
            CHAMS_LOG("gl: program %u writes gl_FragDepth — glDepthRange is INERT for "
                      "it, so a replayed copy lands at the original's depth and is "
                      "occluded by the same walls.", prog);
        }

        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_classified_programs.insert(prog);
            g_seen_programs.erase(prog);
            if (is_entity) {
                g_entity_programs.insert(prog);
                flag_entity_program(prog);
                c_entity_programs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (is_entity) {
            CHAMS_LOG("gl: program %u classified ENTITY by deferred source read-back", prog);

            // First N entity programs of the session -- dump the full source so
            // the log carries the actual GLSL responsible for player/armour
            // rendering. The Lunar vs Modrinth diff falls out of comparing the
            // two dumps directly. Uses the same slot table as the rescue-path
            // dumper, so entity dumps and rescue dumps share the 8-print budget.
            if (prog < kMaxProgId
                && g_source_dumps_left.load(std::memory_order_relaxed) > 0) {
                unsigned char e = 0;
                if (g_prog_source_dumped[prog].compare_exchange_strong(
                        e, 1, std::memory_order_relaxed)) {
                    g_source_dumps_left.fetch_sub(1, std::memory_order_relaxed);

                    GLuint dshaders[8] = { 0 };
                    GLsizei dn = 0;
                    q_GetAttachedShaders(prog, 8, &dn, dshaders);
                    CHAMS_LOG("shaderdump: ENTITY program %u has %d shader(s). "
                              "Dumping source...", (unsigned)prog, (int)dn);
                    for (GLsizei si = 0; si < dn; ++si) {
                        GLint slen = 0;
                        q_GetShaderiv(dshaders[si], kShaderSourceLength, &slen);
                        if (slen <= 0) continue;
                        if (slen > (1 << 14)) slen = (1 << 14);
                        std::string ssrc((size_t)slen, '\0');
                        GLsizei sgot = 0;
                        q_GetShaderSource(dshaders[si], slen, &sgot, ssrc.data());
                        if (sgot <= 0) continue;
                        ssrc.resize((size_t)sgot);

                        const size_t chunk = 1200;
                        size_t off = 0, part = 0;
                        while (off < ssrc.size()) {
                            const size_t take = std::min(chunk, ssrc.size() - off);
                            CHAMS_LOG("shaderdump[prog=%u shader=%u part=%zu]: %.*s",
                                      (unsigned)prog, (unsigned)dshaders[si], part,
                                      (int)take, ssrc.data() + off);
                            off  += take;
                            part += 1;
                        }
                        CHAMS_LOG("shaderdump[prog=%u shader=%u end]: %zu bytes total",
                                  (unsigned)prog, (unsigned)dshaders[si], ssrc.size());
                    }
                }
            }
        }
    }

    // Drain anything our own queries raised so it can never reach the game's
    // next glGetError — under Sodium that would be an in-driver abort.
    CHAMS_MARK("gl::classify_pending_programs -> drain glGetError");
    while (glGetError() != GL_NO_ERROR) { /* drain */ }

    g_suppressed.store(prev, std::memory_order_release);
}

// Create the 1x1 white texture once, at a frame boundary on the game's context.
// Never from a draw detour: this issues GL calls, and under a synchronous debug
// context a GL error there is an in-driver abort.
void ensure_colour_texture()
{
    if (!q_GenTextures || !o_BindTexture || !o_TexImage2D) return;
    if (g_colour_tex && !g_colour_dirty.load(std::memory_order_acquire)) return;
    if (!wglGetCurrentContext()) return;

    const bool prev = g_suppressed.exchange(true, std::memory_order_acq_rel);
    const GLuint restore = g_bound_tex[0].load(std::memory_order_relaxed);

    CHAMS_MARK("gl::ensure_colour_texture -> glGenTextures");
    if (!g_colour_tex) q_GenTextures(1, &g_colour_tex);
    if (g_colour_tex) {
        auto q = [](float v) -> unsigned char {
            if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f;
            return static_cast<unsigned char>(v * 255.f + 0.5f);
        };
        const unsigned char px[4] = {
            q(g_col_r.load(std::memory_order_relaxed)),
            q(g_col_g.load(std::memory_order_relaxed)),
            q(g_col_b.load(std::memory_order_relaxed)),
            255                                   // always opaque; this IS the chams
        };
        CHAMS_MARK("gl::ensure_colour_texture -> glBindTexture(new)");
        o_BindTexture(GL_TEXTURE_2D, g_colour_tex);
        CHAMS_MARK("gl::ensure_colour_texture -> glTexImage2D");
        o_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        CHAMS_MARK("gl::ensure_colour_texture -> glBindTexture(restore)");
        o_BindTexture(GL_TEXTURE_2D, restore);
        g_colour_dirty.store(false, std::memory_order_release);
        CHAMS_LOG("gl: flat-colour texture %u = (%u,%u,%u)", g_colour_tex, px[0], px[1], px[2]);
    }
    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    g_suppressed.store(prev, std::memory_order_release);
}

void frame_boundary()
{
    g_self_drawn_this_frame = false;

    // The first-person item shader owns its per-frame work (heartbeat, window
    // reset, program build, blur + composite). Runs on the game context here,
    // which is where GL is safe to issue and errors are drained.
    ::vanta::modules::itemshader::on_frame_boundary();

    // ONLY when the flat-colour path is actually selected.
    //
    // The colour mode is retired, so g_flat_colour is hardwired false and this
    // texture is never sampled by anything. It was still being created on the
    // first frame after the hooks armed -- issuing glGenTextures + glTexImage2D
    // through MinHook trampolines for opengl32's exports, on the game's context,
    // to build a 1x1 image nothing reads.
    //
    // That is dead work on a per-frame path, which is reason enough. It is also
    // the prime suspect for the 1.21.4 inject crash: the desktop log's last line
    // is the arming message, and the very next chams step is this call, whose
    // own success message never appears even though all three entry points it
    // needs resolved. Suspect, not verdict -- the crash reporter added alongside
    // this will say which.
    if (g_flat_colour.load(std::memory_order_relaxed)) {
        CHAMS_MARK("gl::frame_boundary -> ensure_colour_texture");
        ensure_colour_texture();
    }
    CHAMS_MARK("gl::frame_boundary -> counters");
    const uint64_t now = c_replays.load(std::memory_order_relaxed);
    g_last_frame_replays.store(
        static_cast<int>(now - c_replays_at_frame_start.load(std::memory_order_relaxed)),
        std::memory_order_relaxed);
    c_replays_at_frame_start.store(now, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(g_mx);
    g_last_frame_entity_tex.store(static_cast<int>(g_frame_entity_tex.size()),
                                  std::memory_order_relaxed);
    g_last_frame_entity_tex_set = g_frame_entity_tex;
    g_frame_entity_tex.clear();
    g_last_frame_player_tex.swap(g_frame_player_tex);
    g_frame_player_tex.clear();
}

bool player_texture_was_drawn(unsigned int gl_name)
{
    if (!gl_name) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    return g_last_frame_player_tex.count(static_cast<GLuint>(gl_name)) != 0;
}

bool entity_texture_was_seen(unsigned int gl_name)
{
    if (!gl_name) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    return g_last_frame_entity_tex_set.count(static_cast<GLuint>(gl_name)) != 0;
}

int last_frame_entity_textures()
{
    return g_last_frame_entity_tex.load(std::memory_order_relaxed);
}

int last_frame_replays()
{
    return g_last_frame_replays.load(std::memory_order_relaxed);
}

void set_replay(bool on) { g_replay.store(on, std::memory_order_release); }

void set_diagnostics(bool on) { g_diag.store(on, std::memory_order_release); }
bool diagnostics_on()          { return g_diag.load(std::memory_order_acquire); }

void set_flat_colour(bool on, float r, float g, float b, float a)
{
    g_flat_colour.store(on, std::memory_order_release);

    // Only re-upload when the colour actually moved: this runs every frame and
    // a glTexImage2D per frame would be pure waste.
    auto q8 = [](float v) -> unsigned { if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f;
                                        return static_cast<unsigned>(v * 255.f + 0.5f); };
    const unsigned packed = (q8(r) << 24) | (q8(g) << 16) | (q8(b) << 8) | q8(a);
    if (packed != g_colour_packed.exchange(packed, std::memory_order_acq_rel))
        g_colour_dirty.store(true, std::memory_order_release);

    g_col_r.store(r, std::memory_order_relaxed);
    g_col_g.store(g, std::memory_order_relaxed);
    g_col_b.store(b, std::memory_order_relaxed);
    g_col_a.store(a, std::memory_order_relaxed);
}

bool flat_colour_ready() { return g_colour_tex != 0; }
bool replaying()         { return g_replay.load(std::memory_order_acquire); }
void set_skip_blended(bool on)    { g_skip_blended.store(on, std::memory_order_release); }

void log_frame_summary()
{
    size_t nprog = 0, ntex = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        nprog = g_entity_programs.size();
        ntex  = g_player_textures.size();
    }
    size_t npend = 0, nskin = 0;
    std::string texlist;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        npend = g_seen_programs.size();
        nskin = g_skin_sized_textures.size();
        int shown = 0;
        for (GLuint t : g_entity_draw_textures) {
            if (shown++ >= 12) { texlist += "..."; break; }
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u ", t);
            texlist += buf;
        }
    }
    CHAMS_RL(dbg::kSlotSelfSkip, 2000,
             "gl: texture tracking: bindCalls=%llu activeCalls=%llu perUnit=[%llu %llu %llu %llu] "
             "entityDrawTextures={ %s}",
             c_bind_calls.load(), c_active_calls.load(),
             c_bind_unit[0].load(), c_bind_unit[1].load(),
             c_bind_unit[2].load(), c_bind_unit[3].load(),
             texlist.c_str());

    // Per entry point: total / of which entity. A non-zero
    // glMultiDrawElementsBaseVertex count is Sodium's terrain path, which is
    // direct evidence about whether Sodium is running at all — and therefore
    // about whether its entity culling is what makes players vanish.
    CHAMS_RL(dbg::kSlotRangeSkip, 3000,
             "gl: per-entry draws(entity): DrawElements=%llu(%llu) DrawArrays=%llu(%llu) "
             "BaseVertex=%llu(%llu) Instanced=%llu(%llu) InstBaseVertex=%llu(%llu) "
             "ArraysInst=%llu(%llu) MultiDrawBaseVertex=%llu(%llu) | skippedBlend=%llu "
             "depthTest=%d blend=%d range=[%.3f %.3f] method=%s",
             c_entry[kEDrawElements].load(),                    c_entry_entity[kEDrawElements].load(),
             c_entry[kEDrawArrays].load(),                      c_entry_entity[kEDrawArrays].load(),
             c_entry[kEDrawElementsBaseVertex].load(),          c_entry_entity[kEDrawElementsBaseVertex].load(),
             c_entry[kEDrawElementsInstanced].load(),           c_entry_entity[kEDrawElementsInstanced].load(),
             c_entry[kEDrawElementsInstancedBaseVertex].load(), c_entry_entity[kEDrawElementsInstancedBaseVertex].load(),
             c_entry[kEDrawArraysInstanced].load(),             c_entry_entity[kEDrawArraysInstanced].load(),
             c_entry[kEMultiDrawElementsBaseVertex].load(),     c_entry_entity[kEMultiDrawElementsBaseVertex].load(),
             c_skipped_blend.load(),
             (int)g_depth_test_on.load(), (int)g_blend_on.load(),
             g_depth_near.load(), g_depth_far.load(),
             "depthRange band [0, 0.5] + LEQUAL + depth writes");

    // Whether the depth-state shadow is LIVE or still at this file's defaults.
    // A zero here means the replay is (correctly) refusing to run, and it is
    // the first thing to look at if the chams is silent.
    CHAMS_RL(dbg::kSlotPose, 3000,
             "gl: depth-state shadow: depthFuncCalls=%llu depthMaskCalls=%llu "
             "depthRangeCalls=%llu -> func=0x%04X mask=%d range=[%.4f %.4f] %s%s",
             c_depthfunc_calls.load(), c_depthmask_calls.load(), c_depthrange_calls.load(),
             (unsigned)g_depth_func.load(), (int)g_depth_mask.load(),
             g_depth_near.load(), g_depth_far.load(),
             (c_depthfunc_calls.load() && c_depthmask_calls.load()) ? "LIVE" : "DEFAULTED",
             g_shadow_seeded.load() ? " +SEEDED" : "");
    char equip_ids[128], arm_progs[64];
    {
        int o = 0;
        const int np = g_prog_armour_n.load(std::memory_order_relaxed);
        for (int i = 0; i < np && i < 8 && o < (int)sizeof(arm_progs) - 8; ++i)
            o += std::snprintf(arm_progs + o, sizeof(arm_progs) - o, "%u,",
                               g_prog_armour[i].load(std::memory_order_relaxed));
        arm_progs[o > 0 ? o - 1 : 0] = '\0';
    }
    {
        int o = 0;
        const int ne = g_equip_n.load(std::memory_order_relaxed);
        for (int i = 0; i < ne && i < kMaxUnknown && o < (int)sizeof(equip_ids) - 12; ++i)
            o += std::snprintf(equip_ids + o, sizeof(equip_ids) - o, "%u,",
                               g_equip_id[i].load(std::memory_order_relaxed));
        equip_ids[o > 0 ? o - 1 : 0] = '\0';
    }

    CHAMS_RL(dbg::kSlotFaces, 2000,
             "gl: draws=%llu entity=%llu player=%llu replays=%llu suppressed=%llu | "
             "shaders=%llu linked=%llu entityPrograms=%zu pendingClassify=%zu playerTex=%zu | "
             "skinSized=%zu curProg=%u tex0=%u replayArmed=%d | "
             "sizeFilter[%s]: rejected=%llu maxRejected=%d minKept=%d thresh=%d | "
             "heldItemDraws=%llu (ofWhichSkinBound=%llu) selfDepth=%llu armourDraws=%llu "
             "| playersBeforeSelf=%llu afterSelf=%llu unaccountedEntityTex=%d "
             "equipDraws[known=%llu unknown=%llu ids:%s] "
             "ARMOUR[anyDraw=%llu rescuedFromNonEntityProg=%llu progs:%s]",
             c_draws.load(), c_entity_draws.load(), c_player_draws.load(),
             c_replays.load(), c_suppressed.load(),
             c_shaders_seen.load(), c_programs_linked.load(),
             nprog, npend, ntex, nskin,
             g_current_program.load(), g_bound_tex[0].load(),
             (int)g_replay.load(),
             g_size_filter.load(std::memory_order_relaxed) ? "on" : "OFF",
             c_reject_small.load(),
             c_max_rejected.load(),
             c_min_kept.load() == (1 << 30) ? -1 : c_min_kept.load(),
             (int)kMinPlayerIndices,
             c_held_item_draws.load(), c_held_via_skin.load(), c_self_depth.load(),
             c_armour_draws.load(),
             c_players_before_self.load(), c_players_after_self.load(),
             g_unknown_n.load(),
             c_equip_known.load(), c_equip_unknown.load(), equip_ids,
             c_armour_any_draw.load(), c_armour_other_prog.load(), arm_progs);
}

} // namespace vanta::modules::chams::gl
