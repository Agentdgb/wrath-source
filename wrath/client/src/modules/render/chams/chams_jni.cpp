#include "chams_jni.h"
#include "chams_debug.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace vanta::modules::chams::jni {

namespace {

// ── Intermediary names (Yarn 1.21.11 runtime namespace) ──────────────────────
constexpr const char* kMc      = "net/minecraft/class_310";   // MinecraftClient
constexpr const char* kWorld   = "net/minecraft/class_638";   // ClientWorld
constexpr const char* kPlayer  = "net/minecraft/class_1657";  // PlayerEntity
constexpr const char* kLiving  = "net/minecraft/class_1309";  // LivingEntity
constexpr const char* kEntity  = "net/minecraft/class_1297";  // Entity
constexpr const char* kRtc     = "net/minecraft/class_9779";  // RenderTickCounter
constexpr const char* kLimbAnim= "net/minecraft/class_8080";  // LimbAnimator
constexpr const char* kStack   = "net/minecraft/class_1799";  // ItemStack
constexpr const char* kArm     = "net/minecraft/class_1306";  // Arm
constexpr const char* kHand    = "net/minecraft/class_1268";  // Hand
constexpr const char* kAcp     = "net/minecraft/class_742";   // AbstractClientPlayerEntity
constexpr const char* kPle     = "net/minecraft/class_640";   // PlayerListEntry
constexpr const char* kSkinTex = "net/minecraft/class_8685";  // SkinTextures
// The skin-model enum's own class name is probed at runtime (two shapes across
// the version range) — see the slim-arms block in ensure_cache().

struct cache {
    bool tried = false;
    bool ok    = false;

    jclass    mc_class     = nullptr;   // global
    jmethodID mc_getInstance = nullptr;
    jmethodID mc_getRenderTickCounter = nullptr;
    jmethodID mc_getCameraEntity = nullptr;   // method_1560, optional
    jfieldID  mc_world = nullptr, mc_player = nullptr;
    jmethodID rtc_getTickProgress = nullptr;

    jmethodID world_getPlayers = nullptr;
    jmethodID list_size = nullptr, list_get = nullptr;

    jmethodID ent_getX = nullptr, ent_getY = nullptr, ent_getZ = nullptr;
    jmethodID ent_isAlive = nullptr;

    // Entity.getId(). The exact answer to "is this entity me", and the only
    // per-entity key that survives the trip to the render thread on 1.21.9+.
    // Everything else the module had was frame-global.
    jmethodID ent_getId = nullptr;
    jmethodID ent_isInSneakingPose = nullptr;
    jfieldID  ent_lastRX = nullptr, ent_lastRY = nullptr, ent_lastRZ = nullptr;
    jfieldID  ent_pitch = nullptr, ent_lastPitch = nullptr;
    jfieldID  ent_age = nullptr;

    jfieldID  liv_bodyYaw = nullptr, liv_lastBodyYaw = nullptr;
    jfieldID  liv_headYaw = nullptr, liv_lastHeadYaw = nullptr;
    jfieldID  liv_limbAnimator = nullptr;
    jmethodID la_getProgress = nullptr, la_getAmplitude = nullptr;
    jmethodID liv_getSwing = nullptr;
    jmethodID liv_getMainArm = nullptr;
    jmethodID liv_getMainStack = nullptr, liv_getOffStack = nullptr;
    jmethodID liv_canSee = nullptr;

    /* Tipping the whole body over — see pose_input::lean_rad. All optional: a
       miss leaves a gliding player drawn upright, which is what happened before
       any of this existed. method_6128 and method_6003 keep the same
       intermediary id across 1.21..1.21.11 even though Yarn renamed them from
       FallFlying to Gliding, and the id is what is resolved here. */
    jmethodID liv_leaningPitch = nullptr;   // method_6024 (F)F
    jmethodID liv_isGliding    = nullptr;   // method_6128 ()Z
    jmethodID liv_glidingTicks = nullptr;   // method_6003 ()I
    jmethodID ent_swimmingPose = nullptr;   // method_20232 ()Z
    jmethodID ent_inWater      = nullptr;   // method_5799 ()Z
    jmethodID stack_isEmpty = nullptr;

    jobject   arm_right = nullptr;   // global, Arm.RIGHT
    /* Which hand last swung. Vanilla's getPreferredArm is the main arm, or its
       OPPOSITE when this says the off hand — so a shield raise or a totem pop
       animates the other arm entirely. Optional; a miss falls back to the main
       arm, which is what this did unconditionally before. */
    jfieldID liv_prefHand = nullptr;
    jobject  hand_main    = nullptr;
    jobject   skin_slim = nullptr;   // global, SkinTextures.Model.SLIM (may be null)
    jmethodID acp_getPLE = nullptr, ple_getSkin = nullptr, skin_model = nullptr;

    // ── Skin Identifier -> GL texture name ───────────────────────────────────
    // SkinTextures changed SHAPE at 1.21.9: before it exposed the Identifier
    // directly (comp_1626 -> Identifier); from 1.21.9 it returns an
    // AssetInfo$TextureAsset whose texturePath() (comp_3627) carries it. Both
    // are probed and whichever resolves is used, with the winner logged.
    jmethodID skin_texture_direct = nullptr;   // SkinTextures.texture()  -> Identifier   (<1.21.9)
    jmethodID skin_body_asset     = nullptr;   // SkinTextures.body()     -> TextureAsset (1.21.9+)
    jmethodID asset_texturePath   = nullptr;   // TextureAsset.texturePath() -> Identifier
    jmethodID mc_getTextureManager = nullptr;  // MinecraftClient.getTextureManager()
    jmethodID tm_getTexture        = nullptr;  // TextureManager.getTexture(Identifier)
    // Two eras of "give me the GL name", exactly as the ESP resolves them.
    jmethodID abstex_getGlTexture  = nullptr;  // AbstractTexture.getGlTexture() (1.21.5+)
    jmethodID gltex_getGlId        = nullptr;  // GlTexture.getGlId()
    jmethodID abstex_getGlId       = nullptr;  // AbstractTexture.getGlId()     (1.21..1.21.4)
    int       gl_route = 0;                    // 0 none, 1 legacy int, 2 GpuTexture

    // Identifier.of(namespace, path) -- method_60655, byte-identical across the
    // whole 1.21.0..1.21.11 range in this repo's cached Yarn tables. Used to
    // name the ARMOUR textures, which have no per-entity accessor the way a
    // skin does: they are fixed assets, so they are addressed by path.
    jclass    ident_class = nullptr;            // global ref
    jmethodID ident_of2   = nullptr;            // static Identifier of(String,String)

    // tr7zw EntityCulling — Cullable.setCulled(boolean), mixed into Entity.
    jmethodID ec_setCulled = nullptr;
};

cache C{};

std::atomic<bool> g_defeat_culling{ false };

// The local player, published for the render thread.
//
// The poll already knew who "self" was -- it used it for skip_self and then
// dropped it. Nothing outside the poll could answer the question, which is why
// chams_cull.cpp fell back to IsInstanceOf(PlayerEntity) and caught the local
// player along with everyone else.
std::atomic<int>          g_self_id{ -1 };
std::atomic<unsigned int> g_self_skin{ 0 };

// Last known local-player render position, for the staleness check in
// local_player_pos(). Written on the render thread only.
double g_self_x = 0.0, g_self_y = 0.0, g_self_z = 0.0;
bool   g_self_ok = false;

// Report a lookup and return whether it worked, so the log names the exact
// member that moved rather than leaving a silent null in the cache.
template <class T>
T report(T id, const char* kind, const char* owner, const char* name, const char* desc)
{
    ::vanta::desklog::jni_lookup(dbg::ch(), kind, owner, name, desc, id != nullptr);
    return id;
}

float wrap_deg(float d)
{
    while (d < -180.f) d += 360.f;
    while (d >  180.f) d -= 360.f;
    return d;
}

float lerp_angle(float last, float cur, float t)
{
    return last + wrap_deg(cur - last) * t;
}

// Resolve one player's skin to the GL texture name Minecraft binds when it
// draws their body. Returns 0 when any link in the chain is missing — a miss
// costs the per-player narrowing for that player, never correctness.
//
// mc must be the MinecraftClient instance (already a local ref held by the
// caller) so this does not re-fetch it per player.
unsigned int resolve_skin_gl(JNIEnv* env, jobject mc, jobject entity)
{
    if (!C.acp_getPLE || !C.ple_getSkin || !C.mc_getTextureManager || !C.tm_getTexture
        || C.gl_route == 0)
        return 0;

    unsigned int gl_name = 0;
    jobject ple = env->CallObjectMethod(entity, C.acp_getPLE);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (!ple) return 0;

    if (jobject skin = env->CallObjectMethod(ple, C.ple_getSkin)) {
        jobject ident = nullptr;
        if (C.skin_texture_direct) {
            ident = env->CallObjectMethod(skin, C.skin_texture_direct);
        } else if (C.skin_body_asset && C.asset_texturePath) {
            if (jobject asset = env->CallObjectMethod(skin, C.skin_body_asset)) {
                ident = env->CallObjectMethod(asset, C.asset_texturePath);
                env->DeleteLocalRef(asset);
            }
        }
        if (env->ExceptionCheck()) { env->ExceptionClear(); ident = nullptr; }

        if (ident) {
            if (jobject tmgr = env->CallObjectMethod(mc, C.mc_getTextureManager)) {
                if (jobject tex = env->CallObjectMethod(tmgr, C.tm_getTexture, ident)) {
                    if (C.gl_route == 2) {
                        if (jobject gt = env->CallObjectMethod(tex, C.abstex_getGlTexture)) {
                            gl_name = static_cast<unsigned int>(
                                env->CallIntMethod(gt, C.gltex_getGlId));
                            env->DeleteLocalRef(gt);
                        }
                    } else {
                        gl_name = static_cast<unsigned int>(
                            env->CallIntMethod(tex, C.abstex_getGlId));
                    }
                    env->DeleteLocalRef(tex);
                }
                env->DeleteLocalRef(tmgr);
            }
            env->DeleteLocalRef(ident);
        }
        env->DeleteLocalRef(skin);
    }
    env->DeleteLocalRef(ple);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return gl_name;
}


// ─────────────────────────────────────────────────────────────────────────────
// ARMOUR TEXTURES.
//
// A skin is reachable per player (PlayerListEntry -> SkinTextures). Armour is
// not: it is a fixed set of assets shared by everyone wearing that material, so
// it has to be addressed by path. Both layouts Minecraft has used across the
// supported range are probed, and the one that resolves wins:
//
//   1.21.0 .. 1.21.4   textures/models/armor/<material>_layer_{1,2}.png
//   1.21.5 +           textures/entity/equipment/humanoid[_leggings]/<material>.png
//
// TextureManager.getTexture REGISTERS a placeholder for a path that does not
// exist rather than returning null, and that placeholder is one shared GL
// object. Flagging it would mark every missing-texture draw in the game as
// player armour, so its id is resolved FIRST from a deliberately bogus path and
// excluded from the results.
unsigned int texture_gl_by_path(JNIEnv* env, jobject mc, jobject tmgr,
                                const char* ns, const char* path)
{
    if (!C.ident_class || !C.ident_of2 || !C.tm_getTexture || C.gl_route == 0) return 0;

    jstring jns = env->NewStringUTF(ns);
    jstring jpa = env->NewStringUTF(path);
    unsigned int gl_name = 0;

    if (jns && jpa) {
        jobject ident = env->CallStaticObjectMethod(C.ident_class, C.ident_of2, jns, jpa);
        if (env->ExceptionCheck()) { env->ExceptionClear(); ident = nullptr; }
        if (ident) {
            if (jobject tex = env->CallObjectMethod(tmgr, C.tm_getTexture, ident)) {
                if (C.gl_route == 2) {
                    if (jobject gt = env->CallObjectMethod(tex, C.abstex_getGlTexture)) {
                        gl_name = static_cast<unsigned int>(env->CallIntMethod(gt, C.gltex_getGlId));
                        env->DeleteLocalRef(gt);
                    }
                } else {
                    gl_name = static_cast<unsigned int>(env->CallIntMethod(tex, C.abstex_getGlId));
                }
                env->DeleteLocalRef(tex);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(ident);
        }
    }
    if (jns) env->DeleteLocalRef(jns);
    if (jpa) env->DeleteLocalRef(jpa);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return gl_name;
}

// Names checked against the real assets rather than guessed.
//
// Three entries were dead weight: "golden" (the asset is "gold"), "turtle" (it
// is "turtle_scute", already listed) and "armadillo_scute" (that is wolf body
// armour, not humanoid). Each dead name is not harmless -- TextureManager
// REGISTERS a fresh placeholder for a path that does not exist and hands back a
// distinct GL id, so the "missing" guard below cannot catch them and each one
// flags an id nothing ever binds.
//
// "leather_overlay" was missing and matters: leather declares TWO humanoid
// layers, so without it the dyed overlay of a leather chestplate is never
// flagged, never replayed, and is then deleted by the band the body wrote.
//
// Trims are still not here and cannot be: they are drawn from the armor_trims
// sprite ATLAS, not a per-trim png. Only bracketing the emission covers those,
// which is what the queue path does on 1.21.9+.
// Every name here was read out of the 1.21.11 jar, not guessed. The humanoid
// armour textures that actually ship are exactly:
//   entity/equipment/humanoid/          chainmail copper diamond gold iron
//                                       leather leather_overlay netherite
//                                       turtle_scute
//   entity/equipment/humanoid_leggings/ the same, minus turtle_scute
// Earlier revisions of this list carried three names that exist nowhere
// (golden, turtle, armadillo_scute -- the last is wolf body armour) and omitted
// leather_overlay, which is the dyed second layer of every leather piece.
const char* const kMaterials[] = {
    "leather", "leather_overlay", "chainmail", "iron", "gold", "diamond",
    "netherite", "turtle_scute", "copper",
};

} // namespace

int default_skin_textures(JNIEnv* env, unsigned int* out, int cap)
{
    if (!C.ok || !out || cap <= 0) return 0;
    if (!C.ident_class || !C.ident_of2 || !C.mc_getTextureManager) return 0;

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); mc = nullptr; }
    if (!mc) return 0;
    jobject tmgr = env->CallObjectMethod(mc, C.mc_getTextureManager);
    if (env->ExceptionCheck()) { env->ExceptionClear(); tmgr = nullptr; }
    if (!tmgr) { env->DeleteLocalRef(mc); return 0; }

    const unsigned int missing =
        texture_gl_by_path(env, mc, tmgr, "glr", "textures/__chams_probe_missing__.png");

    int n = 0;
    auto add = [&](unsigned int id) {
        if (!id || (missing && id == missing)) return;
        for (int i = 0; i < n; ++i) if (out[i] == id) return;
        if (n < cap) out[n++] = id;
    };

    // The nine names are not a guess: every one of them was read back out of the
    // live TextureManager in the session that produced this fix.
    static const char* const kNames[] = {
        "steve", "alex", "ari", "efe", "kai", "makena", "noor", "sunny", "zuri",
    };
    char buf[192];
    for (const char* nm : kNames) {
        std::snprintf(buf, sizeof(buf), "textures/entity/player/wide/%s.png", nm);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
        std::snprintf(buf, sizeof(buf), "textures/entity/player/slim/%s.png", nm);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
    }

    CHAMS_RL(dbg::kSlotLiveModel, 10000,
             "default skins resolved: %d of 18", n);

    env->DeleteLocalRef(tmgr);
    env->DeleteLocalRef(mc);
    return n;
}

int equipment_textures(JNIEnv* env, unsigned int* out, int cap)
{
    if (!C.ok || !out || cap <= 0) return 0;
    if (!C.ident_class || !C.ident_of2 || !C.mc_getTextureManager) return 0;

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); mc = nullptr; }
    if (!mc) return 0;
    jobject tmgr = env->CallObjectMethod(mc, C.mc_getTextureManager);
    if (env->ExceptionCheck()) { env->ExceptionClear(); tmgr = nullptr; }
    if (!tmgr) { env->DeleteLocalRef(mc); return 0; }

    // The id every unresolvable path collapses to. Anything equal to this is
    // the missing-texture placeholder, not armour.
    const unsigned int missing =
        texture_gl_by_path(env, mc, tmgr, "glr", "textures/__chams_probe_missing__.png");

    int n = 0;
    auto add = [&](unsigned int id) {
        if (!id || (missing && id == missing)) return;
        for (int i = 0; i < n; ++i) if (out[i] == id) return;   // dedupe
        if (n < cap) out[n++] = id;
    };

    char buf[192];
    for (const char* m : kMaterials) {
        if (n >= cap) break;
        // 1.21.5+ layout
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "textures/entity/equipment/humanoid/%s.png", m);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "textures/entity/equipment/humanoid_leggings/%s.png", m);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
        // 1.21.0 .. 1.21.4 layout
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "textures/models/armor/%s_layer_1.png", m);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "textures/models/armor/%s_layer_2.png", m);
        add(texture_gl_by_path(env, mc, tmgr, "minecraft", buf));
    }

    // The enchantment glint. It is drawn as a second pass over the piece it sits
    // on, masked by glDepthFunc(EQUAL) -- so it paints only where the depth
    // matches what that piece already left behind. When the piece is replayed
    // into the band and the glint is not, there is nothing for the equality to
    // match and the shimmer disappears with it.
    //
    // Two names across the supported range; both are probed and whichever
    // exists is kept.
    add(texture_gl_by_path(env, mc, tmgr, "minecraft",
                           "textures/misc/enchanted_glint_armor.png"));
    add(texture_gl_by_path(env, mc, tmgr, "minecraft",
                           "textures/misc/enchanted_glint_item.png"));
    // Pre-1.21.5 name for the same thing.
    add(texture_gl_by_path(env, mc, tmgr, "minecraft",
                           "textures/misc/enchanted_item_glint.png"));

    // The elytra. It is equipment worn in the chest slot and its own texture,
    // so identity reaches it -- but it was never on the list, which meant it
    // went un-replayed and was then deleted by the band the body wrote.
    add(texture_gl_by_path(env, mc, tmgr, "minecraft",
                           "textures/entity/equipment/wings/elytra.png"));
    add(texture_gl_by_path(env, mc, tmgr, "minecraft",
                           "textures/entity/elytra.png"));

    env->DeleteLocalRef(tmgr);
    env->DeleteLocalRef(mc);
    if (env->ExceptionCheck()) env->ExceptionClear();

    ::vanta::desklog::logf_once(dbg::ch(), 41,
        "armour textures resolved: %d (missing-placeholder id = %u, excluded)",
        n, missing);
    return n;
}

namespace {

} // namespace

int self_entity_id()          { return g_self_id.load(std::memory_order_relaxed); }
unsigned int self_skin_gl()   { return g_self_skin.load(std::memory_order_relaxed); }

bool cache_ready() { return C.ok; }

void set_defeat_entity_culling(bool on)
{
    g_defeat_culling.store(on, std::memory_order_release);
}

bool entity_culling_defeat_available() { return C.ec_setCulled != nullptr; }

bool local_player_pos(double& x, double& y, double& z)
{
    if (!g_self_ok) return false;
    x = g_self_x; y = g_self_y; z = g_self_z;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool ensure_cache(JNIEnv* env)
{
    if (C.ok) return true;
    if (!env) return false;

    ::vanta::desklog::section(dbg::ch(), "JNI cache resolution");

    jclass mc     = ::vanta::jvm::findClassAnyLoader(env, kMc);
    jclass world  = ::vanta::jvm::findClassAnyLoader(env, kWorld);
    jclass living = ::vanta::jvm::findClassAnyLoader(env, kLiving);
    jclass entity = ::vanta::jvm::findClassAnyLoader(env, kEntity);
    jclass rtc    = ::vanta::jvm::findClassAnyLoader(env, kRtc);

    ::vanta::desklog::jni_lookup(dbg::ch(), "class", kMc,     "<self>", "", mc     != nullptr);
    ::vanta::desklog::jni_lookup(dbg::ch(), "class", kWorld,  "<self>", "", world  != nullptr);
    ::vanta::desklog::jni_lookup(dbg::ch(), "class", kLiving, "<self>", "", living != nullptr);
    ::vanta::desklog::jni_lookup(dbg::ch(), "class", kEntity, "<self>", "", entity != nullptr);
    ::vanta::desklog::jni_lookup(dbg::ch(), "class", kRtc,    "<self>", "", rtc    != nullptr);

    if (!mc || !world || !living || !entity || !rtc) {
        // Normal before a world exists; the caller retries.
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    C.mc_class = static_cast<jclass>(env->NewGlobalRef(mc));

    C.mc_getInstance = report(
        env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;"),
        "smthd", kMc, "method_1551", "()LMinecraftClient;");
    C.mc_getRenderTickCounter = report(
        env->GetMethodID(mc, "method_61966", "()Lnet/minecraft/class_9779;"),
        "method", kMc, "method_61966", "()LRenderTickCounter;");
    C.mc_world = report(
        env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;"),
        "field", kMc, "field_1687", "LClientWorld;");
    C.mc_player = report(
        env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;"),
        "field", kMc, "field_1724", "LClientPlayerEntity;");
    C.rtc_getTickProgress = report(
        env->GetMethodID(rtc, "method_60637", "(Z)F"),
        "method", kRtc, "method_60637", "(Z)F");
    // Optional: without it the camera-entity skip below simply never fires.
    C.mc_getCameraEntity = env->GetMethodID(mc, "method_1560", "()Lnet/minecraft/class_1297;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    C.world_getPlayers = report(
        env->GetMethodID(world, "method_18456", "()Ljava/util/List;"),
        "method", kWorld, "method_18456", "()Ljava/util/List;");

    if (jclass lst = env->FindClass("java/util/List")) {
        C.list_size = env->GetMethodID(lst, "size", "()I");
        C.list_get  = env->GetMethodID(lst, "get", "(I)Ljava/lang/Object;");
        env->DeleteLocalRef(lst);
    }

    C.ent_getX    = report(env->GetMethodID(entity, "method_23317", "()D"), "method", kEntity, "method_23317", "()D");
    C.ent_getY    = report(env->GetMethodID(entity, "method_23318", "()D"), "method", kEntity, "method_23318", "()D");
    C.ent_getZ    = report(env->GetMethodID(entity, "method_23321", "()D"), "method", kEntity, "method_23321", "()D");
    C.ent_isAlive = report(env->GetMethodID(entity, "method_5805", "()Z"),  "method", kEntity, "method_5805", "()Z");
    // Optional on purpose: if it does not resolve, self_entity_id() keeps
    // returning -1, which equals no real entity id, and every consumer falls
    // back to the behaviour it had before this existed.
    C.ent_getId   = env->GetMethodID(entity, "method_5628", "()I");
    if (env->ExceptionCheck()) env->ExceptionClear();
    ::vanta::desklog::jni_lookup(dbg::ch(), "method", kEntity, "method_5628",
                                 "()I  Entity.getId", C.ent_getId != nullptr);
    C.ent_isInSneakingPose = report(env->GetMethodID(entity, "method_18276", "()Z"), "method", kEntity, "method_18276", "()Z");

    C.ent_lastRX = report(env->GetFieldID(entity, "field_6038", "D"), "field", kEntity, "field_6038", "D");
    C.ent_lastRY = report(env->GetFieldID(entity, "field_5971", "D"), "field", kEntity, "field_5971", "D");
    C.ent_lastRZ = report(env->GetFieldID(entity, "field_5989", "D"), "field", kEntity, "field_5989", "D");
    C.ent_pitch     = report(env->GetFieldID(entity, "field_5965", "F"), "field", kEntity, "field_5965", "F");
    C.ent_lastPitch = report(env->GetFieldID(entity, "field_6004", "F"), "field", kEntity, "field_6004", "F");
    C.ent_age       = report(env->GetFieldID(entity, "field_6012", "I"), "field", kEntity, "field_6012", "I");

    C.liv_bodyYaw     = report(env->GetFieldID(living, "field_6283", "F"), "field", kLiving, "field_6283", "F");
    C.liv_lastBodyYaw = report(env->GetFieldID(living, "field_6220", "F"), "field", kLiving, "field_6220", "F");
    C.liv_headYaw     = report(env->GetFieldID(living, "field_6241", "F"), "field", kLiving, "field_6241", "F");
    C.liv_lastHeadYaw = report(env->GetFieldID(living, "field_6259", "F"), "field", kLiving, "field_6259", "F");
    C.liv_limbAnimator= report(env->GetFieldID(living, "field_42108", "Lnet/minecraft/class_8080;"),
                               "field", kLiving, "field_42108", "LLimbAnimator;");
    C.liv_getSwing    = report(env->GetMethodID(living, "method_6055", "(F)F"), "method", kLiving, "method_6055", "(F)F");
    C.liv_getMainArm  = report(env->GetMethodID(living, "method_6068", "()Lnet/minecraft/class_1306;"),
                               "method", kLiving, "method_6068", "()LArm;");
    C.liv_getMainStack= report(env->GetMethodID(living, "method_6047", "()Lnet/minecraft/class_1799;"),
                               "method", kLiving, "method_6047", "()LItemStack;");
    C.liv_getOffStack = report(env->GetMethodID(living, "method_6079", "()Lnet/minecraft/class_1799;"),
                               "method", kLiving, "method_6079", "()LItemStack;");
    C.liv_canSee      = report(env->GetMethodID(living, "method_6057", "(Lnet/minecraft/class_1297;)Z"),
                               "method", kLiving, "method_6057", "(LEntity;)Z");

    C.liv_leaningPitch = env->GetMethodID(living, "method_6024", "(F)F");
    C.liv_isGliding    = env->GetMethodID(living, "method_6128", "()Z");
    C.liv_glidingTicks = env->GetMethodID(living, "method_6003", "()I");
    C.ent_swimmingPose = env->GetMethodID(entity, "method_20232", "()Z");
    C.ent_inWater      = env->GetMethodID(entity, "method_5799", "()Z");
    if (env->ExceptionCheck()) env->ExceptionClear();
    ::vanta::desklog::jni_lookup(dbg::ch(), "method", kLiving, "method_6024",
                                 "(F)F (leaningPitch)", C.liv_leaningPitch != nullptr);
    ::vanta::desklog::jni_lookup(dbg::ch(), "method", kLiving, "method_6128",
                                 "()Z (isGliding)", C.liv_isGliding != nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass la = ::vanta::jvm::findClassAnyLoader(env, kLimbAnim)) {
        C.la_getProgress  = report(env->GetMethodID(la, "method_48572", "(F)F"), "method", kLimbAnim, "method_48572", "(F)F");
        C.la_getAmplitude = report(env->GetMethodID(la, "method_48570", "(F)F"), "method", kLimbAnim, "method_48570", "(F)F");
        env->DeleteLocalRef(la);
    }
    if (jclass st = ::vanta::jvm::findClassAnyLoader(env, kStack)) {
        C.stack_isEmpty = report(env->GetMethodID(st, "method_7960", "()Z"), "method", kStack, "method_7960", "()Z");
        env->DeleteLocalRef(st);
    }
    C.liv_prefHand = env->GetFieldID(living, "field_6266", "Lnet/minecraft/class_1268;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    ::vanta::desklog::jni_lookup(dbg::ch(), "field", kLiving, "field_6266",
                                 "LHand; (preferredHand)", C.liv_prefHand != nullptr);

    if (jclass hand = ::vanta::jvm::findClassAnyLoader(env, kHand)) {
        if (jfieldID f = env->GetStaticFieldID(hand, "field_5808", "Lnet/minecraft/class_1268;")) {
            if (jobject o = env->GetStaticObjectField(hand, f)) {
                C.hand_main = env->NewGlobalRef(o);
                env->DeleteLocalRef(o);
            }
        }
        ::vanta::desklog::jni_lookup(dbg::ch(), "sfield", kHand, "field_5808",
                                     "LHand; (MAIN_HAND)", C.hand_main != nullptr);
        env->DeleteLocalRef(hand);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass arm = ::vanta::jvm::findClassAnyLoader(env, kArm)) {
        if (jfieldID f = env->GetStaticFieldID(arm, "field_6183", "Lnet/minecraft/class_1306;")) {
            if (jobject o = env->GetStaticObjectField(arm, f)) {
                C.arm_right = env->NewGlobalRef(o);
                env->DeleteLocalRef(o);
            }
        }
        ::vanta::desklog::jni_lookup(dbg::ch(), "sfield", kArm, "field_6183", "LArm; (RIGHT)", C.arm_right != nullptr);
        env->DeleteLocalRef(arm);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Slim ("Alex") arms. Optional — a miss costs one model unit of arm width,
    // not the feature.
    //
    // The skin-model enum changed SHAPE across the support window, not just
    // name: it is the nested SkinTextures$Model (class_8685$class_7920) on
    // 1.21..1.21.8 and the top-level PlayerSkinType (class_7920) only from
    // 1.21.9. Asking for one form alone means the getter's descriptor never
    // matches and SLIM never resolves, so every player is drawn with classic
    // 4/16 arms — a full model pixel of fill hanging outside a slim skin on
    // each side. So probe both and build the descriptors from whichever class
    // actually exists. (The first cut of this file asked only for the nested
    // form, with the wrong member names on top; the desktop log caught it as
    // the single FAIL in an otherwise clean 36/37.)
    if (jclass acp = ::vanta::jvm::findClassAnyLoader(env, kAcp)) {
        C.acp_getPLE = env->GetMethodID(acp, "method_3123", "()Lnet/minecraft/class_640;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(acp);
    }
    if (jclass ple = ::vanta::jvm::findClassAnyLoader(env, kPle)) {
        C.ple_getSkin = env->GetMethodID(ple, "method_52810", "()Lnet/minecraft/class_8685;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(ple);
    }
    {
        static const char* kSkinTypeNames[] = {
            "net/minecraft/class_7920",            // 1.21.9+      PlayerSkinType
            "net/minecraft/class_8685$class_7920", // 1.21..1.21.8 SkinTextures$Model
        };
        for (const char* tn : kSkinTypeNames) {
            jclass ty = ::vanta::jvm::findClassAnyLoader(env, tn);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!ty) continue;

            char fsig[128], msig[128];
            std::snprintf(fsig, sizeof fsig, "L%s;",   tn);
            std::snprintf(msig, sizeof msig, "()L%s;", tn);

            jmethodID getter = nullptr;
            if (jclass sk = ::vanta::jvm::findClassAnyLoader(env, kSkinTex)) {
                getter = env->GetMethodID(sk, "comp_1629", msig);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(sk);
            }
            jfieldID slimf = env->GetStaticFieldID(ty, "field_41122", fsig);  // SLIM
            if (env->ExceptionCheck()) env->ExceptionClear();

            if (getter && slimf) {
                if (jobject o = env->GetStaticObjectField(ty, slimf)) {
                    C.skin_model = getter;
                    C.skin_slim  = env->NewGlobalRef(o);
                    env->DeleteLocalRef(o);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(ty);
            if (C.skin_slim) { CHAMS_LOG("skin type resolved via %s", tn); break; }
        }
    }
    ::vanta::desklog::jni_lookup(dbg::ch(), "opt", "slim-arms", "chain",
                                 "acp.getPlayerListEntry -> getSkinTextures -> model == SLIM",
                                 C.acp_getPLE && C.ple_getSkin && C.skin_model && C.skin_slim);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // ── Skin Identifier -> GL texture name ───────────────────────────────────
    //
    // This is what lets the native GL path tell a PLAYER draw from a mob, an
    // item frame or the first-person hand. Without it every entity draw is
    // replayed, which is what the log has been reporting as entity == player.
    if (jclass sk = ::vanta::jvm::findClassAnyLoader(env, kSkinTex)) {
        // Pre-1.21.9: the record component is the Identifier itself.
        C.skin_texture_direct = env->GetMethodID(sk, "comp_1626", "()Lnet/minecraft/class_2960;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        // 1.21.9+: it is an AssetInfo$TextureAsset that carries the path.
        if (!C.skin_texture_direct) {
            C.skin_body_asset = env->GetMethodID(
                sk, "comp_1626", "()Lnet/minecraft/class_12079$class_12081;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(sk);
    }
    if (C.skin_body_asset) {
        if (jclass ta = ::vanta::jvm::findClassAnyLoader(
                env, "net/minecraft/class_12079$class_12081")) {
            C.asset_texturePath = env->GetMethodID(ta, "comp_3627",
                                                   "()Lnet/minecraft/class_2960;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(ta);
        }
    }

    if (jclass mcc = ::vanta::jvm::findClassAnyLoader(env, kMc)) {
        C.mc_getTextureManager = env->GetMethodID(mcc, "method_1531",
                                                  "()Lnet/minecraft/class_1060;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(mcc);
    }
    if (jclass idc = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2960")) {
        C.ident_of2 = env->GetStaticMethodID(
            idc, "method_60655",
            "(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (C.ident_of2) C.ident_class = static_cast<jclass>(env->NewGlobalRef(idc));
        ::vanta::desklog::jni_lookup(dbg::ch(), "method", "net/minecraft/class_2960",
                                     "method_60655", "(String,String)Identifier",
                                     C.ident_of2 != nullptr);
        env->DeleteLocalRef(idc);
    }
    if (jclass tm = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1060")) {
        C.tm_getTexture = env->GetMethodID(tm, "method_4619",
                              "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1044;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(tm);
    }
    if (jclass abstex = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1044")) {
        C.abstex_getGlTexture = env->GetMethodID(abstex, "method_68004",
                                    "()Lcom/mojang/blaze3d/textures/GpuTexture;");   // 1.21.5+
        if (env->ExceptionCheck()) env->ExceptionClear();
        C.abstex_getGlId = env->GetMethodID(abstex, "method_4624", "()I");            // 1.21..1.21.4
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(abstex);
    }
    if (jclass gltex = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_10868")) {
        C.gltex_getGlId = env->GetMethodID(gltex, "method_68427", "()I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(gltex);
    }
    C.gl_route = (C.abstex_getGlTexture && C.gltex_getGlId) ? 2
               : (C.abstex_getGlId ? 1 : 0);

    // ── tr7zw EntityCulling: Cullable.setCulled(boolean) ────────────────────
    //
    // The mixin adds the method straight onto Entity, so ask Entity first; the
    // interface is the fallback in case a future build only implements it
    // there. Absent mod => both miss => the call site simply never fires.
    if (jclass ent2 = ::vanta::jvm::findClassAnyLoader(env, kEntity)) {
        C.ec_setCulled = env->GetMethodID(ent2, "setCulled", "(Z)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(ent2);
    }
    if (!C.ec_setCulled) {
        if (jclass cull = ::vanta::jvm::findClassAnyLoader(
                env, "dev/tr7zw/entityculling/versionless/access/Cullable")) {
            C.ec_setCulled = env->GetMethodID(cull, "setCulled", "(Z)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cull);
        }
    }
    ::vanta::desklog::jni_lookup(dbg::ch(), "opt", "EntityCulling",
                                 "Cullable.setCulled", "(Z)V", C.ec_setCulled != nullptr);
    CHAMS_LOG("EntityCulling defeat: %s",
              C.ec_setCulled
                  ? "AVAILABLE — players will be marked not-culled and force-visible each frame"
                  : "mod not present (or method moved); nothing to defeat");

    ::vanta::desklog::jni_lookup(dbg::ch(), "opt", "player-skin", "chain",
        C.skin_texture_direct ? "SkinTextures.texture() [pre-1.21.9]"
                              : "SkinTextures.body().texturePath() [1.21.9+]",
        (C.skin_texture_direct || (C.skin_body_asset && C.asset_texturePath))
            && C.mc_getTextureManager && C.tm_getTexture && C.gl_route != 0);
    CHAMS_LOG("skin->GL route: identifier=%s textureManager=%d getTexture=%d glRoute=%s",
              C.skin_texture_direct ? "direct"
              : (C.skin_body_asset && C.asset_texturePath) ? "assetInfo" : "NONE",
              C.mc_getTextureManager != nullptr, C.tm_getTexture != nullptr,
              C.gl_route == 2 ? "GpuTexture [1.21.5+]"
              : C.gl_route == 1 ? "AbstractTexture.getGlId [1.21..1.21.4]" : "NONE");
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(mc);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(living);
    env->DeleteLocalRef(entity);
    env->DeleteLocalRef(rtc);

    // The hard requirements. Everything else degrades to a slightly less
    // faithful pose rather than to nothing.
    C.ok = C.mc_getInstance && C.mc_world && C.world_getPlayers
        && C.list_size && C.list_get
        && C.ent_getX && C.ent_getY && C.ent_getZ
        && C.ent_lastRX && C.ent_lastRY && C.ent_lastRZ
        && C.liv_bodyYaw && C.liv_lastBodyYaw;

    C.tried = true;
    ::vanta::desklog::jni_summary(dbg::ch());
    CHAMS_LOG("cache_ready = %d", (int)C.ok);
    return C.ok;
}

void release_cache(JNIEnv* env)
{
    if (!env) return;
    if (C.mc_class)  { env->DeleteGlobalRef(C.mc_class);  C.mc_class = nullptr; }
    if (C.arm_right) { env->DeleteGlobalRef(C.arm_right); C.arm_right = nullptr; }
    if (C.hand_main) { env->DeleteGlobalRef(C.hand_main); C.hand_main = nullptr; }
    if (C.skin_slim) { env->DeleteGlobalRef(C.skin_slim); C.skin_slim = nullptr; }
    C.ok = false;
    C.tried = false;
}

// ─────────────────────────────────────────────────────────────────────────────
int poll_players(JNIEnv* env,
                 double cam_x, double cam_y, double cam_z,
                 float range, bool include_self, int max_targets,
                 bool want_visibility, bool want_skin,
                 std::vector<player_sample>& out)
{
    out.clear();
    if (!C.ok || !env) return 0;

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (!mc) return 0;

    jobject world = env->GetObjectField(mc, C.mc_world);
    jobject self  = C.mc_player ? env->GetObjectField(mc, C.mc_player) : nullptr;

    /* @whoever the camera is inside is not a target.

       Vanilla never draws the camera entity in first person, and this poll is
       what the chams and the outline build their players from — so while
       Spectate has the camera in somebody else's head, that somebody would be
       drawn wrapped around the lens at zero distance. Only a FOREIGN camera
       entity is skipped here; your own body stays governed by include_self. */
    jobject cam = C.mc_getCameraEntity ? env->CallObjectMethod(mc, C.mc_getCameraEntity) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); cam = nullptr; }
    if (cam && self && env->IsSameObject(cam, self) == JNI_TRUE) {
        env->DeleteLocalRef(cam);
        cam = nullptr;
    }

    if (!world) {
        CHAMS_RL(dbg::kSlotNoWorld, 2000, "poll: world is null (not in a world yet)");
        if (cam)  env->DeleteLocalRef(cam);
        if (self) env->DeleteLocalRef(self);
        env->DeleteLocalRef(mc);
        return 0;
    }

    // Tick progress: the sub-tick fraction the renderer is interpolating at.
    float tp = 0.f;
    if (C.mc_getRenderTickCounter && C.rtc_getTickProgress) {
        if (jobject rtc = env->CallObjectMethod(mc, C.mc_getRenderTickCounter)) {
            tp = env->CallFloatMethod(rtc, C.rtc_getTickProgress, JNI_TRUE);
            env->DeleteLocalRef(rtc);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    tp = std::clamp(tp, 0.f, 1.f);

    // Local player's live position — the reference the captured camera is
    // checked against.
    g_self_ok = false;
    if (self && C.ent_getX && C.ent_getY && C.ent_getZ) {
        const double sx = env->CallDoubleMethod(self, C.ent_getX);
        const double sy = env->CallDoubleMethod(self, C.ent_getY);
        const double sz = env->CallDoubleMethod(self, C.ent_getZ);
        if (!env->ExceptionCheck()) {
            g_self_x = sx; g_self_y = sy; g_self_z = sz; g_self_ok = true;
        } else {
            env->ExceptionClear();
        }
    }

    jobject list = env->CallObjectMethod(world, C.world_getPlayers);
    if (env->ExceptionCheck()) { env->ExceptionClear(); list = nullptr; }

    int seen = 0, kept = 0;
    if (list) {
        const jint n = env->CallIntMethod(list, C.list_size);
        if (env->ExceptionCheck()) env->ExceptionClear();

        const float range_sq = range * range;

        for (jint i = 0; i < n; ++i) {
            jobject ent = env->CallObjectMethod(list, C.list_get, i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!ent) continue;
            ++seen;

            const bool is_self = self && env->IsSameObject(ent, self) == JNI_TRUE;
            if (is_self) {
                // Record who we are while the object is in hand. Both values
                // are needed: the id identifies the entity being submitted,
                // the GL name catches the case where a remote player resolves
                // to the very same skin texture object.
                if (C.ent_getId) {
                    const jint sid = env->CallIntMethod(ent, C.ent_getId);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    else g_self_id.store(static_cast<int>(sid), std::memory_order_relaxed);
                }
                g_self_skin.store(resolve_skin_gl(env, mc, ent), std::memory_order_relaxed);
            }
            if (is_self && !include_self) {
                CHAMS_BUMP("skip_self");
                env->DeleteLocalRef(ent);
                continue;
            }
            if (cam && env->IsSameObject(ent, cam) == JNI_TRUE) {
                CHAMS_BUMP("skip_camera");
                env->DeleteLocalRef(ent);
                continue;
            }

            if (C.ent_isAlive && env->CallBooleanMethod(ent, C.ent_isAlive) != JNI_TRUE) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                CHAMS_BUMP("skip_dead");
                env->DeleteLocalRef(ent);
                continue;
            }
            if (env->ExceptionCheck()) env->ExceptionClear();

            // Interpolated RENDER position — lastRender* lerped to the current
            // position by the frame's tick progress. This is the value vanilla
            // draws the entity at; using getX/Y/Z alone makes the chams lag the
            // model by up to one tick and visibly swim around it.
            const double cx = env->CallDoubleMethod(ent, C.ent_getX);
            const double cy = env->CallDoubleMethod(ent, C.ent_getY);
            const double cz = env->CallDoubleMethod(ent, C.ent_getZ);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(ent); continue; }
            const double lx = env->GetDoubleField(ent, C.ent_lastRX);
            const double ly = env->GetDoubleField(ent, C.ent_lastRY);
            const double lz = env->GetDoubleField(ent, C.ent_lastRZ);

            player_sample s{};
            s.render_x = lx + (cx - lx) * tp;
            s.render_y = ly + (cy - ly) * tp;
            s.render_z = lz + (cz - lz) * tp;
            s.is_self  = is_self;

            const double ddx = s.render_x - cam_x;
            const double ddy = s.render_y - cam_y;
            const double ddz = s.render_z - cam_z;
            const double d2  = ddx*ddx + ddy*ddy + ddz*ddz;
            if (d2 > range_sq) {
                CHAMS_BUMP("skip_range");
                env->DeleteLocalRef(ent);
                continue;
            }
            s.dist = static_cast<float>(std::sqrt(d2));

            // ── pose inputs ──────────────────────────────────────────────────
            pose::pose_input& p = s.pose;
            const float body_yaw = lerp_angle(env->GetFloatField(ent, C.liv_lastBodyYaw),
                                              env->GetFloatField(ent, C.liv_bodyYaw), tp);
            p.body_yaw_deg = body_yaw;

            if (C.ent_pitch && C.ent_lastPitch)
                p.head_pitch_deg = lerp_angle(env->GetFloatField(ent, C.ent_lastPitch),
                                              env->GetFloatField(ent, C.ent_pitch), tp);
            if (C.liv_headYaw && C.liv_lastHeadYaw) {
                const float hy = lerp_angle(env->GetFloatField(ent, C.liv_lastHeadYaw),
                                            env->GetFloatField(ent, C.liv_headYaw), tp);
                p.head_yaw_rel_deg = wrap_deg(hy - body_yaw);
            }
            if (C.ent_age)
                p.age_ticks = static_cast<float>(env->GetIntField(ent, C.ent_age)) + tp;

            if (C.liv_limbAnimator && C.la_getProgress && C.la_getAmplitude) {
                if (jobject la = env->GetObjectField(ent, C.liv_limbAnimator)) {
                    p.limb_swing     = env->CallFloatMethod(la, C.la_getProgress,  tp);
                    p.limb_amplitude = env->CallFloatMethod(la, C.la_getAmplitude, tp);
                    env->DeleteLocalRef(la);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            if (C.ent_isInSneakingPose) {
                p.sneaking = env->CallBooleanMethod(ent, C.ent_isInSneakingPose) == JNI_TRUE;
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            /* @lying flat: gliding, swimming, crawling.

               One rotation, two sources of weight. Gliding wins where both are
               live, which is what vanilla does — its elytra branch comes before
               the leaning one and they are mutually exclusive in setupTransforms. */
            {
                float w = 0.f;
                bool  gliding = false;

                if (C.liv_isGliding &&
                    env->CallBooleanMethod(ent, C.liv_isGliding) == JNI_TRUE) {
                    gliding = true;
                    /* Ramps in over about ten ticks, so a launch is not a snap
                       from upright to flat. Without the counter it is treated as
                       fully committed, which is right for all but the first half
                       second of a glide. */
                    float ticks = 20.f;
                    if (C.liv_glidingTicks) {
                        ticks = static_cast<float>(env->CallIntMethod(ent, C.liv_glidingTicks)) + tp;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); ticks = 20.f; }
                    }
                    w = std::clamp(ticks * ticks / 100.f, 0.f, 1.f);
                } else if (C.liv_leaningPitch) {
                    w = env->CallFloatMethod(ent, C.liv_leaningPitch, tp);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); w = 0.f; }
                }
                if (env->ExceptionCheck()) env->ExceptionClear();

                if (w > 0.001f) {
                    bool in_water = gliding;   /* the glide always uses the pitch */
                    if (!gliding && C.ent_inWater) {
                        in_water = env->CallBooleanMethod(ent, C.ent_inWater) == JNI_TRUE;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); in_water = false; }
                    }

                    const float target = in_water ? (90.f + p.head_pitch_deg) : 90.f;
                    p.lean_rad = std::clamp(w, 0.f, 1.f) * target * 0.017453293f;

                    if (!gliding && C.ent_swimmingPose) {
                        p.swim_pose = env->CallBooleanMethod(ent, C.ent_swimmingPose) == JNI_TRUE;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); p.swim_pose = false; }
                    }
                }
            }

            bool right_main = true;
            if (C.liv_getMainArm && C.arm_right) {
                if (jobject a = env->CallObjectMethod(ent, C.liv_getMainArm)) {
                    right_main = env->IsSameObject(a, C.arm_right) == JNI_TRUE;
                    env->DeleteLocalRef(a);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            p.main_arm = right_main ? pose::kRightArm : pose::kLeftArm;

            auto has_item = [&](jmethodID getter) -> bool {
                if (!getter || !C.stack_isEmpty) return false;
                bool has = false;
                if (jobject st = env->CallObjectMethod(ent, getter)) {
                    has = env->CallBooleanMethod(st, C.stack_isEmpty) != JNI_TRUE;
                    env->DeleteLocalRef(st);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
                return has;
            };
            p.main_hand_item = has_item(C.liv_getMainStack);
            p.off_hand_item  = has_item(C.liv_getOffStack);

            if (C.liv_getSwing) {
                p.swing_progress = env->CallFloatMethod(ent, C.liv_getSwing, tp);
                if (env->ExceptionCheck()) { env->ExceptionClear(); p.swing_progress = 0.f; }

                if (p.swing_progress > 0.001f) {
                    /* getPreferredArm: the main arm, or its OPPOSITE when the
                       last thing swung was the off hand. This used to be
                       hardcoded to the main arm, which animates the wrong limb
                       through every shield raise, totem pop and offhand throw —
                       a full block of error on a limb a quarter of one wide. */
                    bool main_hand = true;
                    if (C.liv_prefHand && C.hand_main) {
                        if (jobject ph = env->GetObjectField(ent, C.liv_prefHand)) {
                            main_hand = env->IsSameObject(ph, C.hand_main) == JNI_TRUE;
                            env->DeleteLocalRef(ph);
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }

                    const int other = (p.main_arm == pose::kRightArm) ? pose::kLeftArm
                                                                      : pose::kRightArm;
                    p.swinging_arm = main_hand ? p.main_arm : other;
                }
            }

            // Slim arms.
            if (C.acp_getPLE && C.ple_getSkin && C.skin_model && C.skin_slim) {
                if (jobject ple = env->CallObjectMethod(ent, C.acp_getPLE)) {
                    if (jobject sk = env->CallObjectMethod(ple, C.ple_getSkin)) {
                        if (jobject md = env->CallObjectMethod(sk, C.skin_model)) {
                            s.slim = env->IsSameObject(md, C.skin_slim) == JNI_TRUE;
                            env->DeleteLocalRef(md);
                        }
                        env->DeleteLocalRef(sk);
                    }
                    env->DeleteLocalRef(ple);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            // Defeat EntityCulling for this player. One void call; it clears
            // the culled flag and arms 1000 ms of forced visibility, so even a
            // dropped frame cannot let them blink out.
            if (C.ec_setCulled && g_defeat_culling.load(std::memory_order_relaxed)) {
                env->CallVoidMethod(ent, C.ec_setCulled, JNI_FALSE);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            // The GL texture name of this player's skin, for the native path's
            // per-player narrowing. Several JNI calls, so only when asked.
            if (want_skin) s.skin_gl = resolve_skin_gl(env, mc, ent);

            // Per-PLAYER line of sight. Not per pixel — see chams.h.
            if (want_visibility && self && C.liv_canSee && !is_self) {
                s.visible = env->CallBooleanMethod(self, C.liv_canSee, ent) == JNI_TRUE;
                if (env->ExceptionCheck()) { env->ExceptionClear(); s.visible = true; }
            } else {
                s.visible = true;
            }

            out.push_back(s);
            ++kept;
            env->DeleteLocalRef(ent);
        }
        env->DeleteLocalRef(list);
    }

    // Nearest first, then cap. Sorting before the cap is what makes the cap
    // "the 32 closest" rather than "whichever 32 the list happened to start
    // with", which on a crowded server is the difference between a useful
    // feature and a random one.
    std::sort(out.begin(), out.end(),
              [](const player_sample& a, const player_sample& b) { return a.dist < b.dist; });
    if (max_targets > 0 && static_cast<int>(out.size()) > max_targets) {
        ::vanta::desklog::bump(dbg::ch(), "skip_over_cap",
                               static_cast<long long>(out.size()) - max_targets);
        out.resize(static_cast<size_t>(max_targets));
    }

    CHAMS_RL(dbg::kSlotPlayerCount, 1000,
             "poll: list=%d seen=%d kept=%d drawn=%d tickProgress=%.3f cam=(%.2f,%.2f,%.2f)",
             (int)(list ? 1 : 0), seen, kept, (int)out.size(), tp, cam_x, cam_y, cam_z);

    if (cam)  env->DeleteLocalRef(cam);
    if (self) env->DeleteLocalRef(self);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(mc);
    return static_cast<int>(out.size());
}

// -- TEXTURE REGISTRY DUMP ----------------------------------------------------
//
// TextureManager keeps a Map<Identifier, AbstractTexture>. Which field that is
// differs by version and cannot be assumed, so every declared field is examined
// and the ones holding a java.util.Map at runtime are walked. On a version
// where the shape changed entirely this prints nothing and breaks nothing.
void dump_texture_registry(JNIEnv* env, const unsigned int* interesting, int n_interesting)
{
    if (!env || !ensure_cache(env) || !C.mc_getTextureManager) return;

    if (!C.mc_class || !C.mc_getInstance) return;
    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); mc = nullptr; }
    if (!mc) return;
    jobject tmgr = env->CallObjectMethod(mc, C.mc_getTextureManager);
    if (env->ExceptionCheck()) { env->ExceptionClear(); tmgr = nullptr; }
    if (!tmgr) { env->DeleteLocalRef(mc); return; }

    jclass cls_Class  = env->FindClass("java/lang/Class");
    jclass cls_Field  = env->FindClass("java/lang/reflect/Field");
    jclass cls_Map    = env->FindClass("java/util/Map");
    jclass cls_Set    = env->FindClass("java/util/Set");
    jclass cls_Iter   = env->FindClass("java/util/Iterator");
    jclass cls_Entry  = env->FindClass("java/util/Map$Entry");
    jclass cls_Object = env->FindClass("java/lang/Object");
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass tm     = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1060");
    jclass cls_Tex = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1044");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!cls_Class || !cls_Field || !cls_Map || !cls_Set || !cls_Iter || !cls_Entry
        || !cls_Object || !tm || !cls_Tex) {
        CHAMS_LOG("texdump: reflection classes unavailable; nothing dumped");
        if (tm) env->DeleteLocalRef(tm);
        if (cls_Tex) env->DeleteLocalRef(cls_Tex);
        env->DeleteLocalRef(tmgr); env->DeleteLocalRef(mc);
        return;
    }

    const jmethodID m_getDeclaredFields =
        env->GetMethodID(cls_Class, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    const jmethodID m_setAccessible = env->GetMethodID(cls_Field, "setAccessible", "(Z)V");
    const jmethodID m_fieldGet =
        env->GetMethodID(cls_Field, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    const jmethodID m_entrySet  = env->GetMethodID(cls_Map, "entrySet", "()Ljava/util/Set;");
    const jmethodID m_iterator  = env->GetMethodID(cls_Set, "iterator", "()Ljava/util/Iterator;");
    const jmethodID m_hasNext   = env->GetMethodID(cls_Iter, "hasNext", "()Z");
    const jmethodID m_next      = env->GetMethodID(cls_Iter, "next", "()Ljava/lang/Object;");
    const jmethodID m_getKey    = env->GetMethodID(cls_Entry, "getKey", "()Ljava/lang/Object;");
    const jmethodID m_getValue  = env->GetMethodID(cls_Entry, "getValue", "()Ljava/lang/Object;");
    const jmethodID m_toString  = env->GetMethodID(cls_Object, "toString", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (!m_getDeclaredFields || !m_setAccessible || !m_fieldGet || !m_entrySet
        || !m_iterator || !m_hasNext || !m_next || !m_getKey || !m_getValue || !m_toString) {
        CHAMS_LOG("texdump: reflection methods unavailable; nothing dumped");
        env->DeleteLocalRef(tm); env->DeleteLocalRef(cls_Tex);
        env->DeleteLocalRef(tmgr); env->DeleteLocalRef(mc);
        return;
    }

    static const char* const kWanted[] = {
        "equipment", "armor", "armour", "glint", "trims", "elytra", "cape", "cosmetic",
    };

    // Bounded on purpose. This walks a map the render thread owns and mutates,
    // from the poll thread; the only honest way to do that is to touch as little
    // of it as possible and stop early.
    constexpr int kMaxEntries = 4000;
    int dumped = 0, total = 0, maps = 0, foreign = 0;
    bool overflowed = false;
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(tm, m_getDeclaredFields);
    if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }

    const jsize nf = fields ? env->GetArrayLength(fields) : 0;
    for (jsize i = 0; i < nf; ++i) {
        jobject f = env->GetObjectArrayElement(fields, i);
        if (!f) continue;
        env->CallVoidMethod(f, m_setAccessible, JNI_TRUE);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }

        jobject val = env->CallObjectMethod(f, m_fieldGet, tmgr);
        if (env->ExceptionCheck()) { env->ExceptionClear(); val = nullptr; }
        env->DeleteLocalRef(f);
        if (!val) continue;
        if (env->IsInstanceOf(val, cls_Map) != JNI_TRUE) { env->DeleteLocalRef(val); continue; }
        ++maps;

        jobject set = env->CallObjectMethod(val, m_entrySet);
        env->DeleteLocalRef(val);
        if (env->ExceptionCheck()) { env->ExceptionClear(); set = nullptr; }
        if (!set) continue;
        jobject it = env->CallObjectMethod(set, m_iterator);
        env->DeleteLocalRef(set);
        if (env->ExceptionCheck()) { env->ExceptionClear(); it = nullptr; }
        if (!it) continue;

        while (env->CallBooleanMethod(it, m_hasNext) == JNI_TRUE) {
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            if (total >= kMaxEntries) { overflowed = true; break; }
            jobject e = env->CallObjectMethod(it, m_next);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            if (!e) continue;
            ++total;

            jobject k = env->CallObjectMethod(e, m_getKey);
            jobject v = env->CallObjectMethod(e, m_getValue);
            if (env->ExceptionCheck()) env->ExceptionClear();

            char name[256] = { 0 };
            if (k) {
                if (jstring js = (jstring)env->CallObjectMethod(k, m_toString)) {
                    if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                        std::snprintf(name, sizeof(name), "%s", c);
                        env->ReleaseStringUTFChars(js, c);
                    }
                    env->DeleteLocalRef(js);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            unsigned int gl = 0;
            bool is_texture = false;
            if (v && env->IsInstanceOf(v, cls_Tex) == JNI_TRUE) is_texture = true;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (is_texture) {
                if (C.gl_route == 2 && C.abstex_getGlTexture && C.gltex_getGlId) {
                    if (jobject gt = env->CallObjectMethod(v, C.abstex_getGlTexture)) {
                        gl = (unsigned int)env->CallIntMethod(gt, C.gltex_getGlId);
                        env->DeleteLocalRef(gt);
                    }
                } else if (C.abstex_getGlId) {
                    gl = (unsigned int)env->CallIntMethod(v, C.abstex_getGlId);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            } else if (v) {
                ++foreign;
            }

            // Print an entry when its NAME looks like equipment, or when its ID
            // is one the draw path could not account for. That second half is
            // the point of the whole probe: it turns "id 47 gets bound and we do
            // not know what it is" into a filename.
            bool want = false;
            for (const char* kw : kWanted)
                if (std::strstr(name, kw)) { want = true; break; }
            if (!want && gl) {
                for (int j = 0; j < n_interesting; ++j)
                    if (interesting[j] == gl) { want = true; break; }
            }
            if (want && dumped < 96) {
                ++dumped;
                CHAMS_LOG("texdump: gl=%-5u %s%s", gl, name,
                          is_texture ? "" : "   [NOT an AbstractTexture]");
            }

            if (k) env->DeleteLocalRef(k);
            if (v) env->DeleteLocalRef(v);
            env->DeleteLocalRef(e);
        }
        env->DeleteLocalRef(it);
    }
    if (fields) env->DeleteLocalRef(fields);

    CHAMS_LOG("texdump: %d map field(s) on TextureManager, %d entries%s, %d printed, "
              "%d values were NOT AbstractTexture (%d unaccounted ids were asked about)",
              maps, total, overflowed ? " (truncated)" : "", dumped, foreign, n_interesting);

    env->DeleteLocalRef(cls_Tex);
    env->DeleteLocalRef(tm);
    env->DeleteLocalRef(tmgr);
    env->DeleteLocalRef(mc);
}

} // namespace vanta::modules::chams::jni

