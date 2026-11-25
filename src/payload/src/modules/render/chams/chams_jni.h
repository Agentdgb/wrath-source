// chams_jni.h — this module's own JNI cache and per-frame player sampling.
//
// Deliberately its own cache rather than the PlayerESP's. The two modules would
// otherwise share a lifetime and an init order, and neither could be changed or
// disabled without reasoning about the other. The cost is a second set of
// GetFieldID calls at startup — a few microseconds, once.
//
// Every lookup is reported to the desktop debug log through desklog::jni_lookup,
// so a version where something moved names itself in the log instead of
// presenting as "the module does nothing".
//
// All intermediary names below are the ones the shipping PlayerESP resolves at
// runtime today (verified by reading src/modules/render/esp/esp.cpp, which
// documents them as verified against Yarn 1.21.11). They are NOT in the
// generated mapping tables — tools/genmaps/selectors/dt.yml carries no
// renderer or model selectors — so this module resolves them by raw
// intermediary name exactly as the ESP does.

#pragma once

#include "chams_pose.h"

#include <jni.h>
#include <vector>

namespace dt::modules::chams::jni {

// One player, sampled once per frame, in the form the pose and geometry code
// wants. Nothing JNI-shaped survives past poll_players().
struct player_sample {
    double render_x = 0.0, render_y = 0.0, render_z = 0.0;  // interpolated
    pose::pose_input pose{};
    bool   slim    = false;
    bool   is_self = false;
    float  dist    = 0.f;      // to the camera, for sorting and the target cap
    bool   visible = false;    // line of sight from the local player

    // GL texture name of this player's skin, or 0 if it could not be resolved.
    //
    // This is what turns the native GL path from "replay every entity" into
    // "replay players". Minecraft binds a player's skin to texture unit 0 for
    // the draw that renders their body, so a draw whose bound texture is in this
    // set is a player's draw — no Minecraft mappings for the renderer needed.
    unsigned int skin_gl = 0;
};

// Resolves classes, methods and fields. Idempotent, cheap after the first
// success, and safe to call before a world exists (it fails and retries).
bool ensure_cache(JNIEnv* env);

// The LOCAL player's interpolated render position, from the most recent
// poll_players() call. Returns false if it was not available.
//
// This exists to falsify one specific suspicion: the captured camera repeats
// the exact same coordinates across many frames, which would mean the shared
// matrix-capture service is publishing a stale frame — and every frustum
// calculation built on it would then be meaningless. Comparing it against the
// player's own live position settles that in one line instead of by argument.
bool local_player_pos(double& x, double& y, double& z);

// Defeat tr7zw's EntityCulling mod for the players we poll.
//
// That mod raycasts from the camera and marks occluded entities culled, so
// Minecraft never issues their draw — and a chams built on replaying draws has
// nothing to replay. It is why players behind blocks vanished from the effect
// while a raised third-person camera brought them back: from up there the
// raycasts reach them.
//
// The lever comes from the mod's own jar, not from guesswork
// (entityculling-fabric-1.9.5-mc1.21.11.jar, disassembled):
//
//   interface dev.tr7zw.entityculling.versionless.access.Cullable
//       void setCulled(boolean)      // CullableMixin: this.culled = v;
//                                    //                if (!v) setTimeout();
//       void setTimeout()            // lasttime = currentTimeMillis() + 1000
//       boolean isForcedVisible()    // lasttime > currentTimeMillis()
//
//   WorldRendererMixin, per entity: if (isForcedVisible()) render;
//                                   else if (isCulled()) skip;
//
// So a single setCulled(false) per player both clears the culled flag AND arms
// one second of forced visibility. One void JNI call per player per frame, no
// hook, no bytecode, and it is a no-op when the mod is absent.
void set_defeat_entity_culling(bool on);

// Whether the EntityCulling mod was found and the lever resolved.
bool entity_culling_defeat_available();
// GL names of the ARMOUR textures Minecraft has loaded, so the replay can cover
// a player's armour as well as their skin. Without these the copy drawn through
// a wall is a bare body, and -- worse -- the body sorts over the armour in the
// open, which is the flat-white-figure artefact that was reported.
//
// Resolved by asset path rather than per entity, because armour textures are
// shared assets with no per-player accessor. Both the 1.21.0..1.21.4 and the
// 1.21.5+ asset layouts are probed. Returns how many ids were written.
int equipment_textures(JNIEnv* env, unsigned int* out, int cap);

// GL names of Minecraft's nine default skins, in both the wide and the slim
// model. Every player without a custom skin renders with one of these, and they
// are SHARED -- so a single id here can be a dozen players. They cannot be
// reached through PlayerListEntry.getSkinTextures() for anyone the tab list
// does not carry, which is why they were missing entirely.
//
// Returns how many ids were written.
int default_skin_textures(JNIEnv* env, unsigned int* out, int cap);

// The local player, as the poll last saw them. -1 / 0 when unknown, and both
// are values no real entity or texture ever takes, so a consumer that cannot
// resolve them simply behaves as it did before they existed.
//
// This exists because "is this me" had no answer outside the poll thread, and
// every route the chams took to the local player traced back to that gap.
int self_entity_id();
unsigned int self_skin_gl();

// -- NAMING WHAT THE GAME ACTUALLY BINDS -------------------------------------
//
// The armour path resolves ids by asking TextureManager for a KNOWN asset path.
// That answers "which id does minecraft:.../iron.png have" and nothing else --
// so when something binds a DIFFERENT texture for armour, the id is simply
// absent from our set and the draw is invisible to us. The symptom is exactly
// "the armour does not render", and no counter on our side can tell that apart
// from "the armour is never drawn at all".
//
// This walks TextureManager's registry the other way round: every identifier it
// holds, with its GL id, so an unknown id seen at draw time gets a NAME. It is
// resolved by REFLECTION over the field types rather than by an intermediary
// field name, because the point is to work on a build whose mappings we may not
// have -- and a probe that needs the answer in advance is no probe.
//
// Diagnostics only. Nothing in the render path consults it.
void dump_texture_registry(JNIEnv* env, const unsigned int* interesting, int n_interesting);

bool cache_ready();
void release_cache(JNIEnv* env);

// Samples nearby players. Returns how many were written to `out`, nearest
// first, capped at max_targets. Never throws; a JNI exception at any point is
// cleared and the player skipped.
//
// `want_skin` resolves player_sample::skin_gl, which costs a walk from the
// entity through PlayerListEntry and SkinTextures to the texture manager --
// several JNI calls per player, every poll. It is what the native replay path
// narrows on and it is useless to a consumer that only wants the pose, so it is
// asked for rather than always paid for. false leaves skin_gl at 0.
int poll_players(JNIEnv* env,
                 double cam_x, double cam_y, double cam_z,
                 float range, bool include_self, int max_targets,
                 bool want_visibility, bool want_skin,
                 std::vector<player_sample>& out);

} // namespace dt::modules::chams::jni
