// esp.cpp — Player ESP engine. See esp.h.

#include "esp.h"
#include "esp_debug.h"

#include "modules/render/hitfx/hitfx_render_context.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/arraylist/font_pool.h"
#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include "modules/render/outline/camera_effect_time.h"
#include "core/social/EntityFriends.h"   // friends are not drawn on
#include "util/Log.h"
#include "vgui/bridge.h"     // isOverlayVisible() — hide the ESP while the panel is open

#include "imgui.h"

#include <jni.h>

#include <windows.h>
#include <gl/GL.h>
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace hf  = vanta::modules::hitfx;
namespace cap = vanta::modules::hitfx::capture;

namespace vanta::modules::esp {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

// ── Verified Yarn 1.21.11 class names (intermediary — the runtime namespace) ──
constexpr const char* kMc      = "net/minecraft/class_310";   // MinecraftClient
constexpr const char* kWorld   = "net/minecraft/class_638";   // ClientWorld
constexpr const char* kPlayer  = "net/minecraft/class_1657";  // PlayerEntity
constexpr const char* kLiving  = "net/minecraft/class_1309";  // LivingEntity
constexpr const char* kEntity  = "net/minecraft/class_1297";  // Entity
constexpr const char* kBox     = "net/minecraft/class_238";   // Box
constexpr const char* kRtc     = "net/minecraft/class_9779";  // RenderTickCounter
// equipment / atlas
constexpr const char* kSlot    = "net/minecraft/class_1304";  // EquipmentSlot
constexpr const char* kStack   = "net/minecraft/class_1799";  // ItemStack
constexpr const char* kRegs    = "net/minecraft/class_7923";  // Registries
constexpr const char* kRegistry= "net/minecraft/class_2378";  // Registry
constexpr const char* kIdent   = "net/minecraft/class_2960";  // Identifier
constexpr const char* kAtlasMgr= "net/minecraft/class_11697"; // AtlasManager
constexpr const char* kTexMgr  = "net/minecraft/class_1060";  // TextureManager
constexpr const char* kAtlas   = "net/minecraft/class_1059";  // SpriteAtlasTexture
constexpr const char* kSprite  = "net/minecraft/class_1058";  // Sprite
constexpr const char* kGlTex   = "net/minecraft/class_10868"; // GlTexture

// ── Core JNI cache (boxes + health + interpolation) ──────────────────────────
struct core_cache {
    bool tried = false, ok = false;
    jclass    mc_class = nullptr, player_class = nullptr, living_class = nullptr;
    jmethodID mc_getInstance = nullptr, mc_getRenderTickCounter = nullptr;
    jmethodID mc_getCameraEntity = nullptr;   // method_1560, optional
    jfieldID  mc_world_fid = nullptr, mc_player_fid = nullptr;
    jmethodID rtc_getTickProgress = nullptr;
    jmethodID world_getEntities = nullptr;
    jmethodID world_getPlayers = nullptr;      // already filtered, far cheaper
    jmethodID list_size = nullptr, list_get = nullptr;
    jmethodID iter_iterator = nullptr, it_hasNext = nullptr, it_next = nullptr;
    jmethodID ent_getX = nullptr, ent_getY = nullptr, ent_getZ = nullptr;
    jmethodID ent_isAlive = nullptr, ent_getBox = nullptr;
    jmethodID ent_isInSneakingPose = nullptr;   // drives the crouch pose
    jmethodID ent_isInSwimmingPose = nullptr;   // swim / crawl pose
    jmethodID ent_isTouchingWater = nullptr;
    jmethodID ent_hasVehicle = nullptr;
    jmethodID ent_isSwimming = nullptr;   // real swimming, not just the pose
    jmethodID liv_getLeaningPitch = nullptr;    // 0..1 swim lean ramp
    jmethodID gr_getCamera = nullptr, cam_isThirdPerson = nullptr;
    jfieldID  ent_lastRX = nullptr, ent_lastRY = nullptr, ent_lastRZ = nullptr;
    jmethodID ent_getLerpedPos = nullptr;                  // legacy lastRender lerp
    jfieldID  vec_x = nullptr, vec_y = nullptr, vec_z = nullptr;
    // MC's OWN per-frame render state — the exact values the skin was drawn
    // with, so the overlay cannot drift from the model at all.
    jmethodID mc_getEntityRenderDispatcher = nullptr;      // ()EntityRenderManager
    jmethodID erm_getAndUpdateRenderState = nullptr;       // (Entity,float)EntityRenderState
    jclass    living_state_class = nullptr;                // LivingEntityRenderState
    jfieldID  rs_x = nullptr, rs_y = nullptr, rs_z = nullptr;
    jfieldID  rs_bodyYaw = nullptr, rs_relHeadYaw = nullptr, rs_pitch = nullptr;
    jfieldID  rs_limbPos = nullptr, rs_limbAmp = nullptr;
    jfieldID  rs_baseScale = nullptr, rs_ageScale = nullptr;
    jfieldID  rs_posOffset = nullptr;                      // EntityRenderState.positionOffset
    jclass    biped_state_class = nullptr;                 // BipedEntityRenderState
    jfieldID  rs_sneaking = nullptr;                       // isInSneakingPose
    jfieldID  box_minX = nullptr, box_minY = nullptr, box_minZ = nullptr;
    jfieldID  box_maxX = nullptr, box_maxY = nullptr, box_maxZ = nullptr;
    jmethodID liv_getHealth = nullptr, liv_getMaxHealth = nullptr;
    jfieldID  liv_bodyYaw = nullptr, liv_lastBodyYaw = nullptr;   // wireframe orient
    jfieldID  liv_limbAnimator = nullptr;                         // walk-swing animator
    jmethodID la_getProgress = nullptr, la_getAmplitude = nullptr;// limbSwing / amount
    jfieldID  liv_headYaw = nullptr, liv_lastHeadYaw = nullptr;   // head turn
    jfieldID  ent_pitch = nullptr, ent_lastPitch = nullptr;       // head tilt
    jfieldID  ent_age = nullptr;                                  // idle "breathing"
    jmethodID acp_getPLE = nullptr, ple_getSkin = nullptr, skin_model = nullptr; // slim
    jobject   slim_obj = nullptr;                                 // PlayerSkinType.SLIM
    // attack swing + held-item arm pose
    jmethodID liv_getSwing = nullptr;                             // getHandSwingProgress(F)F
    jmethodID liv_getMainArm = nullptr;                           // ()Arm
    jfieldID  liv_prefHand = nullptr;                             // preferredHand (Hand)
    jmethodID liv_getMainStack = nullptr, liv_getOffStack = nullptr;
    jmethodID stack_empty = nullptr;                              // ItemStack.isEmpty
    jobject   arm_right = nullptr, hand_main = nullptr;           // enum singletons (global)
};
core_cache C{};

// ── Equipment / atlas JNI cache (optional) ───────────────────────────────────
struct equip_cache {
    bool tried = false, ok = false;
    jmethodID liv_getEquipped = nullptr;      // LivingEntity.getEquippedStack
    jobject   slot_head = nullptr, slot_chest = nullptr, slot_legs = nullptr, slot_feet = nullptr;
    jmethodID stack_isEmpty = nullptr, stack_getItem = nullptr;
    jobject   item_registry = nullptr;        // Registries.ITEM (global ref)
    jmethodID reg_getId = nullptr;
    jmethodID reg_getRawId = nullptr;
    jmethodID id_getNamespace = nullptr, id_getPath = nullptr;
    jclass    ident_class = nullptr;          // for static Identifier.of
    jmethodID id_of = nullptr;
    jmethodID mc_getAtlasManager = nullptr;   // (kept, unused — threw at runtime)
    jmethodID am_getAtlasTexture = nullptr;
    jmethodID mc_getTexMgr = nullptr;         // TextureManager path (reliable)
    jmethodID tm_getTexture = nullptr;
    jobject   items_atlas_id = nullptr;       // ITEMS_ATLAS_TEXTURE (global ref, may be null)
    jobject   block_atlas_id = nullptr;       // BLOCK_ATLAS_TEXTURE (global ref)
    jmethodID atlas_getSprite = nullptr, atlas_getGlTexture = nullptr;
    jmethodID gltex_getGlId = nullptr;
    jclass    gltex_class = nullptr;
    jmethodID spr_minU = nullptr, spr_maxU = nullptr, spr_minV = nullptr, spr_maxV = nullptr;
};
equip_cache E{};

// ── View-bob JNI cache (optional) ────────────────────────────────────────────
struct bob_cache {
    bool tried = false, ok = false;
    jfieldID  mc_gameRenderer = nullptr;      // MinecraftClient.gameRenderer
    jmethodID gr_bobView = nullptr;           // GameRenderer.bobView(MatrixStack,float)
    jmethodID gr_tiltHurt = nullptr;          // GameRenderer.tiltViewWhenHurt(MatrixStack,float)
    jclass    ms_class = nullptr;             // MatrixStack (for NewObject)
    jmethodID ms_ctor = nullptr, ms_peek = nullptr;
    jmethodID entry_getPosMatrix = nullptr;   // MatrixStack.Entry.getPositionMatrix
    jmethodID m4_get = nullptr;               // org.joml.Matrix4f.get(float[])
    // Auto-detect MC's "View Bobbing" option so we only apply the correction on.
    jfieldID  mc_options = nullptr;            // MinecraftClient.options
    jmethodID go_getBobView = nullptr;         // GameOptions.getBobView -> SimpleOption
    jmethodID so_getValue = nullptr;           // SimpleOption.getValue -> Object
    jmethodID bool_value = nullptr;            // Boolean.booleanValue
};
bob_cache Bc{};

bool init_core(JNIEnv* env)
{
    if (C.tried) return C.ok;
    C.tried = true;

    jclass mc     = vanta::jvm::findClassAnyLoader(env, kMc);
    jclass world  = vanta::jvm::findClassAnyLoader(env, kWorld);
    jclass player = vanta::jvm::findClassAnyLoader(env, kPlayer);
    jclass living = vanta::jvm::findClassAnyLoader(env, kLiving);
    jclass entity = vanta::jvm::findClassAnyLoader(env, kEntity);
    jclass box    = vanta::jvm::findClassAnyLoader(env, kBox);
    jclass rtc    = vanta::jvm::findClassAnyLoader(env, kRtc);
    if (!mc || !world || !player || !living || !entity || !box || !rtc) {
        VLOG_WARN("esp", "init_core: a core class did not resolve");
        return false;
    }

    C.mc_getInstance          = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    C.mc_getRenderTickCounter = env->GetMethodID(mc, "method_61966", "()Lnet/minecraft/class_9779;");
    C.mc_world_fid            = env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;");
    C.mc_player_fid           = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
    C.mc_getCameraEntity      = env->GetMethodID(mc, "method_1560", "()Lnet/minecraft/class_1297;");
    if (env->ExceptionCheck()) env->ExceptionClear();   // optional
    C.rtc_getTickProgress     = env->GetMethodID(rtc, "method_60637", "(Z)F");
    C.world_getEntities       = env->GetMethodID(world, "method_18112", "()Ljava/lang/Iterable;");
    C.world_getPlayers        = env->GetMethodID(world, "method_18456", "()Ljava/util/List;");
    if (jclass lst = env->FindClass("java/util/List")) {
        C.list_size = env->GetMethodID(lst, "size", "()I");
        C.list_get  = env->GetMethodID(lst, "get", "(I)Ljava/lang/Object;");
        env->DeleteLocalRef(lst);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    C.ent_getX     = env->GetMethodID(entity, "method_23317", "()D");
    C.ent_getY     = env->GetMethodID(entity, "method_23318", "()D");
    C.ent_getZ     = env->GetMethodID(entity, "method_23321", "()D");
    C.ent_isAlive  = env->GetMethodID(entity, "method_5805", "()Z");
    C.ent_isInSneakingPose = env->GetMethodID(entity, "method_18276", "()Z");
    C.ent_isInSwimmingPose = env->GetMethodID(entity, "method_20232", "()Z");
    C.ent_isTouchingWater  = env->GetMethodID(entity, "method_5799", "()Z");
    C.ent_hasVehicle       = env->GetMethodID(entity, "method_5765", "()Z");
    C.ent_isSwimming       = env->GetMethodID(entity, "method_5681", "()Z");
    C.liv_getLeaningPitch  = env->GetMethodID(living, "method_6024", "(F)F");
    if (jclass grc = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_757")) {
        C.gr_getCamera = env->GetMethodID(grc, "method_19418", "()Lnet/minecraft/class_4184;");
        env->DeleteLocalRef(grc);
    }
    if (jclass cmc = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4184")) {
        C.cam_isThirdPerson = env->GetMethodID(cmc, "method_19333", "()Z");
        env->DeleteLocalRef(cmc);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    C.ent_getBox   = env->GetMethodID(entity, "method_5829", "()Lnet/minecraft/class_238;");
    C.ent_lastRX   = env->GetFieldID(entity, "field_6038", "D");
    C.ent_lastRY   = env->GetFieldID(entity, "field_5971", "D");
    C.ent_lastRZ   = env->GetFieldID(entity, "field_5989", "D");
    // MC's own interpolated render position (what the renderer places entities at).
    C.ent_getLerpedPos = env->GetMethodID(entity, "method_30950", "(F)Lnet/minecraft/class_243;");
    if (jclass v3 = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_243")) {
        C.vec_x = env->GetFieldID(v3, "field_1352", "D");
        C.vec_y = env->GetFieldID(v3, "field_1351", "D");
        C.vec_z = env->GetFieldID(v3, "field_1350", "D");
        env->DeleteLocalRef(v3);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    C.box_minX = env->GetFieldID(box, "field_1323", "D");
    C.box_minY = env->GetFieldID(box, "field_1322", "D");
    C.box_minZ = env->GetFieldID(box, "field_1321", "D");
    C.box_maxX = env->GetFieldID(box, "field_1320", "D");
    C.box_maxY = env->GetFieldID(box, "field_1325", "D");
    C.box_maxZ = env->GetFieldID(box, "field_1324", "D");

    C.liv_getHealth    = env->GetMethodID(living, "method_6032", "()F");
    C.liv_getMaxHealth = env->GetMethodID(living, "method_6063", "()F");
    C.liv_bodyYaw      = env->GetFieldID(living, "field_6283", "F");   // bodyYaw
    C.liv_lastBodyYaw  = env->GetFieldID(living, "field_6220", "F");   // lastBodyYaw
    C.liv_limbAnimator = env->GetFieldID(living, "field_42108", "Lnet/minecraft/class_8080;");
    if (jclass la = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_8080")) {
        C.la_getProgress  = env->GetMethodID(la, "method_48572", "(F)F");  // getAnimationProgress
        C.la_getAmplitude = env->GetMethodID(la, "method_48570", "(F)F");  // getAmplitude
        env->DeleteLocalRef(la);
    }
    C.liv_headYaw      = env->GetFieldID(living, "field_6241", "F");   // headYaw
    C.liv_lastHeadYaw  = env->GetFieldID(living, "field_6259", "F");   // lastHeadYaw
    if (env->ExceptionCheck()) env->ExceptionClear();
    C.ent_pitch        = env->GetFieldID(entity, "field_5965", "F");   // pitch
    C.ent_lastPitch    = env->GetFieldID(entity, "field_6004", "F");   // lastPitch
    C.ent_age          = env->GetFieldID(entity, "field_6012", "I");   // age (breathing)
    // Slim (Alex) detection: AbstractClientPlayerEntity.getPlayerListEntry()
    // -> PlayerListEntry.getSkinTextures() -> SkinTextures.model() vs SLIM.
    if (jclass acp = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_742")) {
        C.acp_getPLE = env->GetMethodID(acp, "method_3123", "()Lnet/minecraft/class_640;");
        env->DeleteLocalRef(acp);
    }
    if (jclass ple = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_640")) {
        C.ple_getSkin = env->GetMethodID(ple, "method_52810", "()Lnet/minecraft/class_8685;");
        env->DeleteLocalRef(ple);
    }
    if (jclass sk = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_8685")) {
        C.skin_model = env->GetMethodID(sk, "comp_1629", "()Lnet/minecraft/class_7920;");
        env->DeleteLocalRef(sk);
    }
    if (jclass ty = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_7920")) {
        jfieldID f = env->GetStaticFieldID(ty, "field_41122", "Lnet/minecraft/class_7920;");  // SLIM
        if (f) { jobject o = env->GetStaticObjectField(ty, f);
                 if (o) { C.slim_obj = env->NewGlobalRef(o); env->DeleteLocalRef(o); } }
        env->DeleteLocalRef(ty);
    }
    // Attack swing + held-item pose.
    C.liv_getSwing     = env->GetMethodID(living, "method_6055", "(F)F");                       // getHandSwingProgress
    C.liv_getMainArm   = env->GetMethodID(living, "method_6068", "()Lnet/minecraft/class_1306;"); // getMainArm
    C.liv_prefHand     = env->GetFieldID(living, "field_6266", "Lnet/minecraft/class_1268;");   // preferredHand
    C.liv_getMainStack = env->GetMethodID(living, "method_6047", "()Lnet/minecraft/class_1799;"); // getMainHandStack
    C.liv_getOffStack  = env->GetMethodID(living, "method_6079", "()Lnet/minecraft/class_1799;"); // getOffHandStack
    if (jclass st = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1799"))
        { C.stack_empty = env->GetMethodID(st, "method_7960", "()Z"); env->DeleteLocalRef(st); }
    if (jclass ar = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1306")) {
        jfieldID f = env->GetStaticFieldID(ar, "field_6183", "Lnet/minecraft/class_1306;");     // Arm.RIGHT
        if (f) { jobject o = env->GetStaticObjectField(ar, f);
                 if (o) { C.arm_right = env->NewGlobalRef(o); env->DeleteLocalRef(o); } }
        env->DeleteLocalRef(ar);
    }
    if (jclass hd = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1268")) {
        jfieldID f = env->GetStaticFieldID(hd, "field_5808", "Lnet/minecraft/class_1268;");     // Hand.MAIN_HAND
        if (f) { jobject o = env->GetStaticObjectField(hd, f);
                 if (o) { C.hand_main = env->NewGlobalRef(o); env->DeleteLocalRef(o); } }
        env->DeleteLocalRef(hd);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass iterable = env->FindClass("java/lang/Iterable");
    jclass iterator = env->FindClass("java/util/Iterator");
    if (iterable) C.iter_iterator = env->GetMethodID(iterable, "iterator", "()Ljava/util/Iterator;");
    if (iterator) {
        C.it_hasNext = env->GetMethodID(iterator, "hasNext", "()Z");
        C.it_next    = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
    }
    // Render-state path (authoritative): MinecraftClient.getEntityRenderDispatcher
    // -> EntityRenderManager.getAndUpdateRenderState(entity, tickProgress).
    C.mc_getEntityRenderDispatcher = env->GetMethodID(mc, "method_1561", "()Lnet/minecraft/class_898;");
    if (jclass erm = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_898")) {
        C.erm_getAndUpdateRenderState = env->GetMethodID(
            erm, "method_72977", "(Lnet/minecraft/class_1297;F)Lnet/minecraft/class_10017;");
        env->DeleteLocalRef(erm);
    }
    if (jclass rs = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_10017")) {
        C.rs_x = env->GetFieldID(rs, "field_53325", "D");
        C.rs_y = env->GetFieldID(rs, "field_53326", "D");
        C.rs_z = env->GetFieldID(rs, "field_53327", "D");
        C.rs_posOffset = env->GetFieldID(rs, "field_53336", "Lnet/minecraft/class_243;");
        env->DeleteLocalRef(rs);
    }
    if (jclass ls = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_10042")) {
        C.rs_bodyYaw     = env->GetFieldID(ls, "field_53446", "F");
        C.rs_relHeadYaw  = env->GetFieldID(ls, "field_53447", "F");
        C.rs_pitch       = env->GetFieldID(ls, "field_53448", "F");
        C.rs_limbPos     = env->GetFieldID(ls, "field_53450", "F");
        C.rs_limbAmp     = env->GetFieldID(ls, "field_53451", "F");
        C.rs_baseScale   = env->GetFieldID(ls, "field_53453", "F");
        C.rs_ageScale    = env->GetFieldID(ls, "field_53454", "F");
        C.living_state_class = static_cast<jclass>(env->NewGlobalRef(ls));
        env->DeleteLocalRef(ls);
    }
    if (jclass bs = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_10034")) {
        C.rs_sneaking = env->GetFieldID(bs, "field_53410", "Z");   // isInSneakingPose
        C.biped_state_class = static_cast<jclass>(env->NewGlobalRef(bs));
        env->DeleteLocalRef(bs);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    C.mc_class     = static_cast<jclass>(env->NewGlobalRef(mc));
    C.player_class = static_cast<jclass>(env->NewGlobalRef(player));
    C.living_class = static_cast<jclass>(env->NewGlobalRef(living));

    env->DeleteLocalRef(mc); env->DeleteLocalRef(world); env->DeleteLocalRef(player);
    env->DeleteLocalRef(living); env->DeleteLocalRef(entity); env->DeleteLocalRef(box);
    env->DeleteLocalRef(rtc);
    if (iterable) env->DeleteLocalRef(iterable);
    if (iterator) env->DeleteLocalRef(iterator);

    C.ok = C.mc_getInstance && C.mc_getRenderTickCounter && C.mc_world_fid && C.mc_player_fid
        && C.rtc_getTickProgress && C.world_getEntities
        && C.ent_getX && C.ent_getY && C.ent_getZ && C.ent_isAlive && C.ent_getBox
        && C.ent_lastRX && C.ent_lastRY && C.ent_lastRZ
        && C.box_minX && C.box_minY && C.box_minZ && C.box_maxX && C.box_maxY && C.box_maxZ
        && C.liv_getHealth && C.liv_getMaxHealth
        && C.iter_iterator && C.it_hasNext && C.it_next
        && C.mc_class && C.player_class && C.living_class;
    VLOG_INFO("esp", "init_core ok=%d", (int)C.ok);
    if (!C.ok) VLOG_WARN("esp", "init_core: some ids unresolved — ESP boxes off");
    return C.ok;
}

// Resolve a static EquipmentSlot constant into a global ref.
jobject slot_ref(JNIEnv* env, jclass slotCls, const char* fieldId)
{
    jfieldID fid = env->GetStaticFieldID(slotCls, fieldId, "Lnet/minecraft/class_1304;");
    if (!fid) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jobject o = env->GetStaticObjectField(slotCls, fid);
    if (!o) return nullptr;
    jobject g = env->NewGlobalRef(o);
    env->DeleteLocalRef(o);
    return g;
}

// Resolve a static Identifier constant (an *_ATLAS_TEXTURE) into a global ref.
jobject atlas_id_ref(JNIEnv* env, jclass atlasCls, const char* fieldId)
{
    jfieldID fid = env->GetStaticFieldID(atlasCls, fieldId, "Lnet/minecraft/class_2960;");
    if (!fid) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jobject o = env->GetStaticObjectField(atlasCls, fid);
    if (!o) return nullptr;
    jobject g = env->NewGlobalRef(o);
    env->DeleteLocalRef(o);
    return g;
}

bool init_equip(JNIEnv* env)
{
    if (E.tried) return E.ok;
    E.tried = true;

    jclass slot   = vanta::jvm::findClassAnyLoader(env, kSlot);
    jclass stack  = vanta::jvm::findClassAnyLoader(env, kStack);
    jclass regs   = vanta::jvm::findClassAnyLoader(env, kRegs);
    jclass reg    = vanta::jvm::findClassAnyLoader(env, kRegistry);
    jclass ident  = vanta::jvm::findClassAnyLoader(env, kIdent);
    jclass amgr   = vanta::jvm::findClassAnyLoader(env, kAtlasMgr);
    jclass texmgr = vanta::jvm::findClassAnyLoader(env, kTexMgr);
    jclass atlas  = vanta::jvm::findClassAnyLoader(env, kAtlas);
    jclass sprite = vanta::jvm::findClassAnyLoader(env, kSprite);
    jclass gltex  = vanta::jvm::findClassAnyLoader(env, kGlTex);
    if (!slot || !stack || !regs || !reg || !ident || !amgr || !texmgr || !atlas || !sprite || !gltex) {
        VLOG_WARN("esp", "init_equip: a class did not resolve — equipment off");
        return false;
    }

    E.liv_getEquipped   = env->GetMethodID(C.living_class, "method_6118",
                                           "(Lnet/minecraft/class_1304;)Lnet/minecraft/class_1799;");
    E.stack_isEmpty     = env->GetMethodID(stack, "method_7960", "()Z");
    E.stack_getItem     = env->GetMethodID(stack, "method_7909", "()Lnet/minecraft/class_1792;");
    E.reg_getId         = env->GetMethodID(reg, "method_10221",
                                           "(Ljava/lang/Object;)Lnet/minecraft/class_2960;");
    E.reg_getRawId      = env->GetMethodID(reg, "method_10206", "(Ljava/lang/Object;)I");
    E.id_getNamespace   = env->GetMethodID(ident, "method_12836", "()Ljava/lang/String;");
    E.id_getPath        = env->GetMethodID(ident, "method_12832", "()Ljava/lang/String;");
    E.id_of             = env->GetStaticMethodID(ident, "method_60655",
                              "(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;");
    E.mc_getAtlasManager= env->GetMethodID(C.mc_class, "method_72703", "()Lnet/minecraft/class_11697;");
    E.am_getAtlasTexture= env->GetMethodID(amgr, "method_73025",
                              "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1059;");
    E.mc_getTexMgr      = env->GetMethodID(C.mc_class, "method_1531", "()Lnet/minecraft/class_1060;");
    E.tm_getTexture     = env->GetMethodID(texmgr, "method_4619",
                              "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1044;");
    E.atlas_getSprite   = env->GetMethodID(atlas, "method_4608",
                              "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1058;");
    E.atlas_getGlTexture= env->GetMethodID(atlas, "method_68004",
                              "()Lcom/mojang/blaze3d/textures/GpuTexture;");
    E.gltex_getGlId     = env->GetMethodID(gltex, "method_68427", "()I");
    E.spr_minU = env->GetMethodID(sprite, "method_4594", "()F");
    E.spr_maxU = env->GetMethodID(sprite, "method_4577", "()F");
    E.spr_minV = env->GetMethodID(sprite, "method_4593", "()F");
    E.spr_maxV = env->GetMethodID(sprite, "method_4575", "()F");
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Registries.ITEM static field (type DefaultedRegistry = class_7922).
    jfieldID itemFid = env->GetStaticFieldID(regs, "field_41178", "Lnet/minecraft/class_7922;");
    if (itemFid) {
        jobject r = env->GetStaticObjectField(regs, itemFid);
        if (r) { E.item_registry = env->NewGlobalRef(r); env->DeleteLocalRef(r); }
    } else if (env->ExceptionCheck()) env->ExceptionClear();

    E.slot_head  = slot_ref(env, slot, "field_6169");
    E.slot_chest = slot_ref(env, slot, "field_6174");
    E.slot_legs  = slot_ref(env, slot, "field_6172");
    E.slot_feet  = slot_ref(env, slot, "field_6166");

    E.items_atlas_id = atlas_id_ref(env, atlas, "field_64467");  // ITEMS_ATLAS_TEXTURE (may be null)
    E.block_atlas_id = atlas_id_ref(env, atlas, "field_5275");   // BLOCK_ATLAS_TEXTURE

    E.ident_class = static_cast<jclass>(env->NewGlobalRef(ident));
    E.gltex_class = static_cast<jclass>(env->NewGlobalRef(gltex));

    env->DeleteLocalRef(slot); env->DeleteLocalRef(stack); env->DeleteLocalRef(regs);
    env->DeleteLocalRef(reg); env->DeleteLocalRef(ident); env->DeleteLocalRef(amgr);
    env->DeleteLocalRef(texmgr);
    env->DeleteLocalRef(atlas); env->DeleteLocalRef(sprite); env->DeleteLocalRef(gltex);

    E.ok = E.liv_getEquipped && E.stack_isEmpty && E.stack_getItem && E.reg_getId
        && E.id_getNamespace && E.id_getPath && E.id_of && E.mc_getTexMgr && E.tm_getTexture
        && E.atlas_getSprite && E.atlas_getGlTexture && E.gltex_getGlId
        && E.spr_minU && E.spr_maxU && E.spr_minV && E.spr_maxV
        && E.item_registry && E.slot_head && E.slot_chest && E.slot_legs && E.slot_feet
        && E.block_atlas_id && E.ident_class && E.gltex_class;
    VLOG_INFO("esp", "init_equip ok=%d (items_atlas=%d)", (int)E.ok, (int)(E.items_atlas_id != nullptr));
    dbg::log("init_equip ok=%d | getEquipped=%p isEmpty=%p getItem=%p getId=%p ns=%p path=%p of=%p "
             "getTexMgr=%p getTexture=%p getSprite=%p getGlTex=%p getGlId=%p U=%p "
             "reg=%p slots[H=%p C=%p L=%p F=%p] itemsId=%p blockId=%p identCls=%p glCls=%p",
             (int)E.ok, (void*)E.liv_getEquipped, (void*)E.stack_isEmpty, (void*)E.stack_getItem,
             (void*)E.reg_getId, (void*)E.id_getNamespace, (void*)E.id_getPath, (void*)E.id_of,
             (void*)E.mc_getTexMgr, (void*)E.tm_getTexture, (void*)E.atlas_getSprite,
             (void*)E.atlas_getGlTexture, (void*)E.gltex_getGlId, (void*)E.spr_minU,
             (void*)E.item_registry, (void*)E.slot_head, (void*)E.slot_chest, (void*)E.slot_legs,
             (void*)E.slot_feet, (void*)E.items_atlas_id, (void*)E.block_atlas_id,
             (void*)E.ident_class, (void*)E.gltex_class);
    if (!E.ok) VLOG_WARN("esp", "init_equip: equipment icons unavailable — falling back");
    return E.ok;
}

// Consumes `js` (deletes the local ref) — callers pass a freshly returned string.
std::string read_jstr(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* p = env->GetStringUTFChars(js, nullptr);
    std::string s = p ? p : "";
    if (p) env->ReleaseStringUTFChars(js, p);
    env->DeleteLocalRef(js);
    return s;
}

inline ImU32 pack_rgba(float r, float g, float b, float a)
{
    auto b8 = [](float x){ x = std::clamp(x, 0.f, 1.f); return (int)(x * 255.f + 0.5f); };
    return IM_COL32(b8(r), b8(g), b8(b), b8(a));
}

// If a JNI exception is pending, log its class + message (rate-limited) and clear.
void log_pending_exc(JNIEnv* env, const char* where)
{
    if (!env->ExceptionCheck()) return;
    jthrowable t = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string cls, msg;
    if (t) {
        jclass tc = env->GetObjectClass(t);
        jclass clsCls = env->FindClass("java/lang/Class");
        jmethodID getName = clsCls ? env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;") : nullptr;
        if (tc && getName) cls = read_jstr(env, (jstring)env->CallObjectMethod(tc, getName));
        jclass thr = env->FindClass("java/lang/Throwable");
        jmethodID getMsg = thr ? env->GetMethodID(thr, "getMessage", "()Ljava/lang/String;") : nullptr;
        if (getMsg) msg = read_jstr(env, (jstring)env->CallObjectMethod(t, getMsg));
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

// ── View-bob: replicate MC's exact bobView so the ESP tracks the bobbing view ─
bool init_bob(JNIEnv* env)
{
    if (Bc.tried) return Bc.ok;
    Bc.tried = true;

    Bc.mc_gameRenderer = env->GetFieldID(C.mc_class, "field_1773", "Lnet/minecraft/class_757;");

    jclass gr    = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_757");
    jclass ms    = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4587");
    jclass entry = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4587$class_4665");
    jclass m4    = env->FindClass("org/joml/Matrix4f");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!gr || !ms || !entry || !m4) {
        VLOG_WARN("esp", "init_bob: a class did not resolve — view-bob off");
        return false;
    }

    Bc.gr_bobView         = env->GetMethodID(gr, "method_3186", "(Lnet/minecraft/class_4587;F)V");
    /* Optional: without it the boxes simply keep the pre-existing behaviour of
       ignoring the damage tilt, rather than the whole correction failing. */
    Bc.gr_tiltHurt        = env->GetMethodID(gr, "method_3198", "(Lnet/minecraft/class_4587;F)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    Bc.ms_ctor            = env->GetMethodID(ms, "<init>", "()V");
    Bc.ms_peek            = env->GetMethodID(ms, "method_23760", "()Lnet/minecraft/class_4587$class_4665;");
    Bc.entry_getPosMatrix = env->GetMethodID(entry, "method_23761", "()Lorg/joml/Matrix4f;");
    Bc.m4_get             = env->GetMethodID(m4, "get", "([F)[F");
    // View-bobbing option (auto-detect).
    Bc.mc_options = env->GetFieldID(C.mc_class, "field_1690", "Lnet/minecraft/class_315;");
    if (jclass go = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_315"))
        { Bc.go_getBobView = env->GetMethodID(go, "method_42448", "()Lnet/minecraft/class_7172;"); env->DeleteLocalRef(go); }
    if (jclass so = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_7172"))
        { Bc.so_getValue = env->GetMethodID(so, "method_41753", "()Ljava/lang/Object;"); env->DeleteLocalRef(so); }
    if (jclass bcl = env->FindClass("java/lang/Boolean"))
        { Bc.bool_value = env->GetMethodID(bcl, "booleanValue", "()Z"); env->DeleteLocalRef(bcl); }
    if (env->ExceptionCheck()) env->ExceptionClear();

    Bc.ms_class = static_cast<jclass>(env->NewGlobalRef(ms));

    env->DeleteLocalRef(gr); env->DeleteLocalRef(ms);
    env->DeleteLocalRef(entry); env->DeleteLocalRef(m4);

    Bc.ok = Bc.mc_gameRenderer && Bc.gr_bobView && Bc.ms_ctor && Bc.ms_peek
         && Bc.entry_getPosMatrix && Bc.m4_get && Bc.ms_class;
    dbg::log("init_bob ok=%d | gr=%p bobView=%p ctor=%p peek=%p getPos=%p m4get=%p",
             (int)Bc.ok, (void*)Bc.mc_gameRenderer, (void*)Bc.gr_bobView, (void*)Bc.ms_ctor,
             (void*)Bc.ms_peek, (void*)Bc.entry_getPosMatrix, (void*)Bc.m4_get);
    if (!Bc.ok) VLOG_WARN("esp", "init_bob: view-bob correction unavailable");
    return Bc.ok;
}

// Run MC's bobView on a fresh MatrixStack and read the resulting matrix (the bob
// transform in isolation, column-major). Returns false on any failure.
bool compute_bob(JNIEnv* env, jobject mc, float tp, float out[16])
{
    // Walking bob is optional; damage tilt is independent of that setting.
    bool bob_enabled = true;
    if (Bc.mc_options && Bc.go_getBobView && Bc.so_getValue && Bc.bool_value) {
        jobject opts = env->GetObjectField(mc, Bc.mc_options);
        bool on = true;
        if (opts) {
            jobject so = env->CallObjectMethod(opts, Bc.go_getBobView);
            if (so) {
                jobject val = env->CallObjectMethod(so, Bc.so_getValue);
                if (val) { on = env->CallBooleanMethod(val, Bc.bool_value) == JNI_TRUE; env->DeleteLocalRef(val); }
                env->DeleteLocalRef(so);
            }
            env->DeleteLocalRef(opts);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        bob_enabled = on;
    }

    jobject grr = env->GetObjectField(mc, Bc.mc_gameRenderer);
    if (!grr) return false;
    jobject stack = env->NewObject(Bc.ms_class, Bc.ms_ctor);
    if (!stack) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
// Match renderWorld: camera interpolation, then hurt tilt followed by walking bob.
    tp = ::vanta::modules::outline::view::camera_effect_time(env, grr, tp);
    if (Bc.gr_tiltHurt) env->CallVoidMethod(grr, Bc.gr_tiltHurt, stack, tp);
    if (bob_enabled)    env->CallVoidMethod(grr, Bc.gr_bobView,  stack, tp);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject entry = env->CallObjectMethod(stack, Bc.ms_peek);
    if (!entry) return false;
    jobject m4 = env->CallObjectMethod(entry, Bc.entry_getPosMatrix);
    if (!m4) return false;
    jfloatArray arr = env->NewFloatArray(16);
    if (!arr) return false;
    env->CallObjectMethod(m4, Bc.m4_get, arr);          // joml Matrix4f.get(float[]) — column-major
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(arr); return false; }
    env->GetFloatArrayRegion(arr, 0, 16, out);
    env->DeleteLocalRef(arr);
    return true;
}

// out = A * B  (both column-major, index c*4 + r).
void mat_mul(const float* A, const float* B, float* out)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += A[k*4 + r] * B[c*4 + k];
            out[c*4 + r] = s;
        }
}

// ── Per-frame MVP + projection (cull only behind camera, so close boxes stay) ─
struct proj_ctx {
    float mvp[16] = {};
    double cx = 0, cy = 0, cz = 0;
    float vw = 0, vh = 0;
    bool valid = false;
};

// bob (optional, column-major) is prepended to the modelview so the projected
// boxes ride the same view-bob the game rendered the world with.
void build_proj(const hf::render_frame& f, const float* bob, proj_ctx& p)
{
    float M2[16];
    if (bob) mat_mul(bob, f.modelview, M2);              // bob * modelview
    else     std::memcpy(M2, f.modelview, sizeof(M2));
    mat_mul(f.projection, M2, p.mvp);                    // projection * (bob*modelview)
    p.cx = f.cam_x; p.cy = f.cam_y; p.cz = f.cam_z;
    p.vw = (float)f.viewport_w; p.vh = (float)f.viewport_h;
    p.valid = f.valid && p.vw > 0 && p.vh > 0;
}

// Project an absolute world point. Returns false ONLY when behind the camera.
bool project(const proj_ctx& p, double wx, double wy, double wz, float& sx, float& sy)
{
    const float x = (float)(wx - p.cx), y = (float)(wy - p.cy), z = (float)(wz - p.cz);
    const float* m = p.mvp;
    const float cx = m[0]*x + m[4]*y + m[8]*z  + m[12];
    const float cy = m[1]*x + m[5]*y + m[9]*z  + m[13];
    const float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    if (cw <= 1e-4f) return false;               // behind camera
    const float ndcx = cx / cw, ndcy = cy / cw;
    sx = (ndcx * 0.5f + 0.5f) * p.vw;
    sy = p.vh - (ndcy * 0.5f + 0.5f) * p.vh;
    return true;
}

// ── Draw targets built each frame ────────────────────────────────────────────
struct EquipIcon {
    bool present = false;
    bool sprite  = false;      // true: real atlas icon; false: fallback box
    unsigned int gl_id = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    char letter = '?';
    ImU32 tint = IM_COL32(200,200,200,255);
};
struct EspTarget {
    bool  box_valid = false;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;      // 2D screen bbox
    bool  corners_valid = false;
    float cxp[8] = {}, cyp[8] = {};                    // projected 3D corners
    bool  wpart[6] = {};                               // part has any usable corner
    bool  wok[6][8] = {};                              // per-corner projection ok
    float wxp[6][8] = {}, wyp[6][8] = {};              // projected part corners
    bool  skel = false;                                // skeleton joints computed
    float sjx[11] = {}, sjy[11] = {}; bool sjok[11] = {}; // projected joints
    bool  has_health = false;
    float health_frac = 1.f;
    EquipIcon equip[4];                                // head, chest, legs, feet
};
std::mutex g_mx;
std::vector<EspTarget> g_targets;

// Live atlas GL texture ids (set by poll, read by draw — same render thread).
unsigned int g_items_gl_cur = 0, g_block_gl_cur = 0;

// Force crisp NEAREST sampling (no mipmaps) on the item atlas while we draw the
// equipment icons, then restore MC's original filtering. Runs as ImGui draw
// callbacks so the GL state change lands mid-draw-list, on the right textures.
struct FilterSave { unsigned int gl[2]; int mn[2], mg[2], mx[2]; int n; };
FilterSave g_fsave{};

void cb_set_nearest(const ImDrawList*, const ImDrawCmd* cmd)
{
    FilterSave* s = static_cast<FilterSave*>(cmd->UserCallbackData);
    if (!s) return;
    for (int i = 0; i < s->n; ++i) {
        if (!s->gl[i]) continue;
        glBindTexture(GL_TEXTURE_2D, s->gl[i]);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &s->mn[i]);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &s->mg[i]);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  &s->mx[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  0);
    }
}

void cb_restore_filter(const ImDrawList*, const ImDrawCmd* cmd)
{
    FilterSave* s = static_cast<FilterSave*>(cmd->UserCallbackData);
    if (!s) return;
    for (int i = 0; i < s->n; ++i) {
        if (!s->gl[i]) continue;
        glBindTexture(GL_TEXTURE_2D, s->gl[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->mn[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->mg[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  s->mx[i]);
    }
}

// Standard Minecraft player model parts as local AABBs (blocks, feet at y=0,
// centred on x/z, +z = model forward). Scaled to the entity height and rotated
// by body yaw. {xmin,ymin,zmin, xmax,ymax,zmax}.
const float WIRE_PARTS[6][6] = {
    { -0.25f, 1.50f, -0.25f,   0.25f, 2.00f,  0.25f  },  // head
    { -0.25f, 0.75f, -0.125f,  0.25f, 1.50f,  0.125f },  // body
    {  0.25f, 0.75f, -0.125f,  0.50f, 1.50f,  0.125f },  // right arm
    { -0.50f, 0.75f, -0.125f, -0.25f, 1.50f,  0.125f },  // left arm
    { -0.00625f, 0.00f, -0.125f,  0.24375f, 0.75f,  0.125f },  // right leg (±2 around 1.9)
    { -0.24375f, 0.00f, -0.125f,  0.00625f, 0.75f,  0.125f },  // left leg
};
// Box edges over corner index (xi*4 + yi*2 + zi).
const int WIRE_EDGES[12][2] = {
    {0,4},{1,5},{2,6},{3,7},   // x edges
    {0,2},{1,3},{4,6},{5,7},   // y edges
    {0,1},{2,3},{4,5},{6,7},   // z edges
};
// Box faces (4 corner indices each) — valid for both the 3D-box corner order and
// the wireframe part corner order (fill draws all six, so the mapping is symmetric).
const int BOX_FACES[6][4] = {
    {0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5},
};

// Animation pivots, taken from BipedEntityModel (model units / 16 = blocks,
// model Y is down from an origin 1.5 blocks above the feet):
//   head (0,0)  body (0,0)  arms (±5,2)  legs (±1.9,12)
// A wrong pivot makes a swinging limb trace the wrong arc — barely visible at
// walking speed, obvious while sprinting (it reads as the wireframe lagging).
const float WIRE_PIVOT[6][2] = {
    {  0.00000f, 1.500f },  // head   — neck
    {  0.00000f, 1.500f },  // body   — waist (used by the sneak lean)
    {  0.31250f, 1.375f },  // right arm — shoulder (5/16, 1.5-2/16)
    { -0.31250f, 1.375f },  // left arm
    {  0.11875f, 0.750f },  // right leg — hip (1.9/16)
    { -0.11875f, 0.750f },  // left leg
};

// Sneak pose (BipedEntityModel.setAngles when isInSneakingPose): parts drop and
// the legs shift back, plus a 0.5 rad body lean and +0.4 rad on both arms.
const float SNEAK_DY[6] = { -0.2625f, -0.2000f, -0.2000f, -0.2000f, -0.0125f, -0.0125f };
// legs shift BACKWARD (MC pivotZ 4/16). +z is forward in our space (derived
// from the held-item arm pose), so the offset is negative here.
const float SNEAK_DZ[6] = {  0.0000f,  0.0000f,  0.0000f,  0.0000f, -0.2500f, -0.2500f };

// Material tint for the fallback icon (when the atlas sprite can't be resolved).
ImU32 tint_for(const std::string& path)
{
    auto has = [&](const char* k){ return path.find(k) != std::string::npos; };
    if (has("netherite")) return IM_COL32( 70, 66, 70,255);
    if (has("diamond"))   return IM_COL32(100,220,220,255);
    if (has("golden") || has("gold")) return IM_COL32(240,205, 70,255);
    if (has("iron"))      return IM_COL32(215,215,220,255);
    if (has("chainmail")) return IM_COL32(150,150,160,255);
    if (has("leather"))   return IM_COL32(160,100, 60,255);
    if (has("turtle"))    return IM_COL32(110,180, 90,255);
    return IM_COL32(200,200,200,255);
}

// An item's icon never changes, so resolving it per player per frame burned
// thousands of JNI calls and Java allocations. Cache by raw item id and drop the
// cache when the atlas texture changes (resource-pack reload).
std::vector<EquipIcon> g_icon_cache;
unsigned int g_icon_cache_items_gl = 0, g_icon_cache_block_gl = 0;

void icon_cache_sync(unsigned int itemsGl, unsigned int blockGl)
{
    if (itemsGl != g_icon_cache_items_gl || blockGl != g_icon_cache_block_gl) {
        g_icon_cache.clear();
        g_icon_cache_items_gl = itemsGl;
        g_icon_cache_block_gl = blockGl;
    }
}

// Resolve one equipment slot into an EquipIcon (real sprite or fallback).
// All object refs are created inside the caller's local frame.
EquipIcon resolve_icon(JNIEnv* env, jobject entity, jobject slotEnum,
                       jobject itemsAtlas, jobject blockAtlas,
                       jobject missItems, jobject missBlock,
                       unsigned int itemsGl, unsigned int blockGl)
{
    EquipIcon ic{};
    if (!slotEnum) return ic;

    jobject stack = env->CallObjectMethod(entity, E.liv_getEquipped, slotEnum);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ic; }
    if (!stack) return ic;
    if (env->CallBooleanMethod(stack, E.stack_isEmpty) == JNI_TRUE) return ic;

    jobject item = env->CallObjectMethod(stack, E.stack_getItem);
    if (!item) return ic;

    // Cache hit: everything below (registry id, string building, atlas lookup) is
    // constant for this item, so pay for it once instead of every frame.
    int rawId = -1;
    if (E.reg_getRawId) {
        rawId = (int)env->CallIntMethod(E.item_registry, E.reg_getRawId, item);
        if (env->ExceptionCheck()) { env->ExceptionClear(); rawId = -1; }
        if (rawId >= 0) {
            if ((int)g_icon_cache.size() <= rawId) g_icon_cache.resize(rawId + 1);
            if (g_icon_cache[rawId].present) return g_icon_cache[rawId];
        }
    }

    jobject id = env->CallObjectMethod(E.item_registry, E.reg_getId, item);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    if (!id) return ic;

    const std::string ns   = read_jstr(env, (jstring)env->CallObjectMethod(id, E.id_getNamespace));
    const std::string path = read_jstr(env, (jstring)env->CallObjectMethod(id, E.id_getPath));
    if (path.empty()) return ic;

    ic.present = true;
    ic.letter  = (char)std::toupper((unsigned char)path[0]);
    ic.tint    = tint_for(path);

    jstring jns = env->NewStringUTF(ns.empty() ? "minecraft" : ns.c_str());
    // Two sprite-id conventions: "item/<path>" (block atlas, historical) and
    // bare "<path>" (the 1.21.11 items atlas). Try each on each atlas.
    const std::string cands[2] = { std::string("item/") + path, path };

    struct Try { jobject atlas; jobject missing; unsigned int gl; const char* tag; };
    const Try tries[2] = { { itemsAtlas, missItems, itemsGl, "items" },
                           { blockAtlas, missBlock, blockGl, "block" } };
    const char* okTag = "none"; std::string okCand;
    for (const Try& t : tries) {
        if (!t.atlas || t.gl == 0) continue;
        for (const std::string& cand : cands) {
            jstring jsp = env->NewStringUTF(cand.c_str());
            jobject spriteId = env->CallStaticObjectMethod(E.ident_class, E.id_of, jns, jsp);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!spriteId) continue;
            jobject spr = env->CallObjectMethod(t.atlas, E.atlas_getSprite, spriteId);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!spr) continue;
            if (t.missing && env->IsSameObject(spr, t.missing)) continue;   // not on this atlas
            ic.u0 = env->CallFloatMethod(spr, E.spr_minU);
            ic.u1 = env->CallFloatMethod(spr, E.spr_maxU);
            ic.v0 = env->CallFloatMethod(spr, E.spr_minV);
            ic.v1 = env->CallFloatMethod(spr, E.spr_maxV);
            ic.gl_id  = t.gl;
            ic.sprite = true;
            okTag = t.tag; okCand = cand;
            break;
        }
        if (ic.sprite) break;
    }
    if (rawId >= 0 && (int)g_icon_cache.size() > rawId) g_icon_cache[rawId] = ic;
    return ic;
}

// ── Poll players, project, build targets ─────────────────────────────────────
void poll(JNIEnv* env, const hf::render_frame& cam)
{
    std::vector<EspTarget> out;
    if (!(cam.valid && cam.viewport_w > 0 && cam.viewport_h > 0)) {
        std::lock_guard<std::mutex> lk(g_mx); g_targets.swap(out); return;
    }

    // One outer local frame bounds all the refs this poll creates (mc/world/
    // atlases/iterator); each entity also gets its own nested frame below.
    if (env->PushLocalFrame(256) != 0) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        std::lock_guard<std::mutex> lk(g_mx); g_targets.swap(out); return;
    }
    auto bail = [&]() { env->PopLocalFrame(nullptr);
                        std::lock_guard<std::mutex> lk(g_mx); g_targets.swap(out); };

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!mc) { bail(); return; }
    jobject world  = env->GetObjectField(mc, C.mc_world_fid);
    jobject self   = env->GetObjectField(mc, C.mc_player_fid);
    if (!world || !self) { bail(); return; }

    /* The entity the camera is inside, when that is somebody OTHER than you
       (Spectate). Vanilla skips it in first person — a box around the lens is
       no use — and draws it in third, which is the rule mirrored below. Freed
       by the outer local frame. */
    jobject camEnt = C.mc_getCameraEntity ? env->CallObjectMethod(mc, C.mc_getCameraEntity) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); camEnt = nullptr; }
    if (camEnt && env->IsSameObject(camEnt, self) == JNI_TRUE) camEnt = nullptr;

    // tick progress (matches WorldRenderer's interpolation).
    float tp = 1.0f;
    jobject rtc = env->CallObjectMethod(mc, C.mc_getRenderTickCounter);
    if (rtc) tp = env->CallFloatMethod(rtc, C.rtc_getTickProgress, JNI_TRUE);
    if (env->ExceptionCheck()) env->ExceptionClear();
    tp = std::clamp(tp, 0.f, 1.f);

    // In first person MC does not draw your own body, so Show Self must not
    // either — otherwise a wireframe torso hangs in front of the camera.
    bool thirdPerson = true;
    if (Bc.mc_gameRenderer && C.gr_getCamera && C.cam_isThirdPerson) {
        jobject grr2 = env->GetObjectField(mc, Bc.mc_gameRenderer);
        if (grr2) {
            jobject camo = env->CallObjectMethod(grr2, C.gr_getCamera);
            if (camo) {
                thirdPerson = env->CallBooleanMethod(camo, C.cam_isThirdPerson) == JNI_TRUE;
                env->DeleteLocalRef(camo);
            }
            env->DeleteLocalRef(grr2);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // View-bob correction: prepend MC's exact bob transform to the modelview so
    // the boxes track players while the view bobs (the original ESP behavior).
    float bob[16]; bool hasBob = false;
    if (init_bob(env))
        hasBob = compute_bob(env, mc, tp, bob);   // gated on MC's option inside

    proj_ctx P{};
    build_proj(cam, hasBob ? bob : nullptr, P);

    const double sx0 = env->CallDoubleMethod(self, C.ent_getX);
    const double sy0 = env->CallDoubleMethod(self, C.ent_getY);
    const double sz0 = env->CallDoubleMethod(self, C.ent_getZ);
    const float range2 = g_settings.range * g_settings.range;

    // Per-frame equipment context (resolved once; reused for every player).
    const bool want_equip = g_settings.equipment && init_equip(env);
    jobject itemsAtlas = nullptr, blockAtlas = nullptr, missItems = nullptr, missBlock = nullptr;
    unsigned int itemsGl = 0, blockGl = 0;
    if (want_equip) {
        // Reach the SpriteAtlasTexture through the TextureManager (the reliable
        // path; AtlasManager.getAtlasTexture throws at runtime in 1.21.11).
        jobject tm = env->CallObjectMethod(mc, E.mc_getTexMgr);
        if (env->ExceptionCheck()) { log_pending_exc(env, "getTextureManager"); tm = nullptr; }
        if (tm) {
            jstring gpath = env->NewStringUTF("item/__vanta_missing_probe__");
            jstring gns   = env->NewStringUTF("minecraft");
            jobject garbageId = env->CallStaticObjectMethod(E.ident_class, E.id_of, gns, gpath);
            if (env->ExceptionCheck()) env->ExceptionClear();
            auto resolve_atlas = [&](const char* tag, jobject atlasId, jobject& atlasOut,
                                     jobject& missOut, unsigned int& glOut) {
                if (!atlasId) { dbg::log_rl(4, 2000, "atlas[%s]: id is null", tag); return; }
                jobject at = env->CallObjectMethod(tm, E.tm_getTexture, atlasId);
                if (env->ExceptionCheck()) { log_pending_exc(env, tag); return; }
                if (!at) { dbg::log_rl(6, 2000, "atlas[%s]: getTexture null", tag); return; }
                atlasOut = at;
                jobject gpu = env->CallObjectMethod(at, E.atlas_getGlTexture);
                if (env->ExceptionCheck()) log_pending_exc(env, "getGlTexture");
                const int isGl = (gpu ? env->IsInstanceOf(gpu, E.gltex_class) : 0);
                if (gpu && isGl == JNI_TRUE) {
                    glOut = (unsigned int)env->CallIntMethod(gpu, E.gltex_getGlId);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); glOut = 0; }
                }
                if (garbageId) {
                    missOut = env->CallObjectMethod(at, E.atlas_getSprite, garbageId);
                    if (env->ExceptionCheck()) log_pending_exc(env, "getSprite/miss");
                }
            };
            resolve_atlas("items", E.items_atlas_id, itemsAtlas, missItems, itemsGl);
            resolve_atlas("block", E.block_atlas_id, blockAtlas, missBlock, blockGl);
            icon_cache_sync(itemsGl, blockGl);
            env->DeleteLocalRef(tm);
        } else {
        }
    }

    // The entity render dispatcher owns the per-entity render states MC draws with.
    jobject erm = nullptr;
    if (C.mc_getEntityRenderDispatcher) {
        erm = env->CallObjectMethod(mc, C.mc_getEntityRenderDispatcher);
        if (env->ExceptionCheck()) { env->ExceptionClear(); erm = nullptr; }
    }

    // ClientWorld.getPlayers() is already filtered to players — walking the full
    // entity list cost a JNI round trip per dropped item, mob and projectile.
    jobject plist = nullptr; jint plist_n = 0; jint plist_i = 0;
    jobject it = nullptr;
    if (C.world_getPlayers && C.list_size && C.list_get) {
        plist = env->CallObjectMethod(world, C.world_getPlayers);
        if (env->ExceptionCheck()) { env->ExceptionClear(); plist = nullptr; }
        if (plist) plist_n = env->CallIntMethod(plist, C.list_size);
    }
    if (!plist) {
        jobject iterable = env->CallObjectMethod(world, C.world_getEntities);
        it = iterable ? env->CallObjectMethod(iterable, C.iter_iterator) : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    int n_players = 0;
    for (;;) {
        jobject ent = nullptr;
        if (plist) {
            if (plist_i >= plist_n) break;
            ent = env->CallObjectMethod(plist, C.list_get, plist_i++);
        } else {
            if (!it || env->CallBooleanMethod(it, C.it_hasNext) != JNI_TRUE) break;
            ent = env->CallObjectMethod(it, C.it_next);
        }
        if (!ent) continue;

        // A failed push must NOT be followed by a pop — that would destroy the
        // caller's frame and invalidate every reference it owns.
        if (env->PushLocalFrame(128) != 0) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(ent);
            continue;
        }

        bool keep = false;
        EspTarget t{};
        do {
            if (env->IsInstanceOf(ent, C.player_class) != JNI_TRUE) break;
            // A friend is not drawn on. Dropped here rather than recoloured
            // downstream so none of the projection work is paid for them.
            if (::vanta::social::isFriend(env, ent)) break;
            if (env->IsSameObject(ent, self) && (!g_settings.showself || !thirdPerson)) break;
            if (camEnt && !thirdPerson && env->IsSameObject(ent, camEnt)) break;
            if (env->CallBooleanMethod(ent, C.ent_isAlive) != JNI_TRUE) break;

            const double cxr = env->CallDoubleMethod(ent, C.ent_getX);
            const double cyr = env->CallDoubleMethod(ent, C.ent_getY);
            const double czr = env->CallDoubleMethod(ent, C.ent_getZ);
            const double dd = (cxr-sx0)*(cxr-sx0) + (cyr-sy0)*(cyr-sy0) + (czr-sz0)*(czr-sz0);
            if (dd > range2) break;
            ++n_players;

            // Interpolated render position (jitter-free — matches WorldRenderer).
            const double lrx = env->GetDoubleField(ent, C.ent_lastRX);
            const double lry = env->GetDoubleField(ent, C.ent_lastRY);
            const double lrz = env->GetDoubleField(ent, C.ent_lastRZ);
            double rx = lrx + (cxr - lrx) * tp;
            double ry = lry + (cyr - lry) * tp;
            double rz = lrz + (czr - lrz) * tp;

            // AUTHORITATIVE: ask MC for the very render state it drew this entity
            // with — position, body yaw, head yaw/pitch and limb swing all come
            // from one place, so the overlay cannot drift from the model.
            bool  haveRS = false;
            float rsBodyYaw = 0.f, rsRelHead = 0.f, rsPitch = 0.f, rsLimbPos = 0.f, rsLimbAmp = 0.f;
            float rsScale = 1.f;
            bool  gotPosOffset = false;
            // Crouch pose straight off the entity — Entity.isInSneakingPose().
            const bool sneaking = C.ent_isInSneakingPose
                               && env->CallBooleanMethod(ent, C.ent_isInSneakingPose) == JNI_TRUE;
            if (env->ExceptionCheck()) env->ExceptionClear();

            // Swim / crawl lean (0..1 ramp), and whether we are in water — the
            // model tips toward horizontal by up to 90 degrees.
            float leanPitch = 0.f;
            bool  swimPose = false, inWater = false;
            if ((g_settings.mode == 2 || g_settings.mode == 3) && C.liv_getLeaningPitch) {
                leanPitch = env->CallFloatMethod(ent, C.liv_getLeaningPitch, tp);
                if (env->ExceptionCheck()) { env->ExceptionClear(); leanPitch = 0.f; }
                if (leanPitch > 0.001f) {
                    swimPose = C.ent_isSwimming
                            && env->CallBooleanMethod(ent, C.ent_isSwimming) == JNI_TRUE;
                    inWater  = C.ent_isTouchingWater
                            && env->CallBooleanMethod(ent, C.ent_isTouchingWater) == JNI_TRUE;
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            }
            const bool riding = (g_settings.mode == 2 || g_settings.mode == 3)
                             && C.ent_hasVehicle
                             && env->CallBooleanMethod(ent, C.ent_hasVehicle) == JNI_TRUE;
            if (env->ExceptionCheck()) env->ExceptionClear();
            // Rebuilding MC's render state is not free — only the poses need it.
            const bool needRS = (g_settings.mode == 2 || g_settings.mode == 3);
            if (needRS && erm && C.erm_getAndUpdateRenderState && C.rs_x) {
                jobject rs = env->CallObjectMethod(erm, C.erm_getAndUpdateRenderState, ent, tp);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (rs) {
                    rx = env->GetDoubleField(rs, C.rs_x);
                    ry = env->GetDoubleField(rs, C.rs_y);
                    rz = env->GetDoubleField(rs, C.rs_z);
                    // Pose offset (sneaking drops the model 0.125 blocks).
                    if (C.rs_posOffset && C.vec_x) {
                        jobject po = env->GetObjectField(rs, C.rs_posOffset);
                        if (po) {
                            rx += env->GetDoubleField(po, C.vec_x);
                            ry += env->GetDoubleField(po, C.vec_y);
                            rz += env->GetDoubleField(po, C.vec_z);
                            gotPosOffset = true;
                            env->DeleteLocalRef(po);
                        }
                    }
                    if (C.living_state_class && C.rs_bodyYaw
                        && env->IsInstanceOf(rs, C.living_state_class) == JNI_TRUE) {
                        rsBodyYaw = env->GetFloatField(rs, C.rs_bodyYaw);
                        rsRelHead = env->GetFloatField(rs, C.rs_relHeadYaw);
                        rsPitch   = env->GetFloatField(rs, C.rs_pitch);
                        rsLimbPos = env->GetFloatField(rs, C.rs_limbPos);
                        rsLimbAmp = env->GetFloatField(rs, C.rs_limbAmp);
                        if (C.rs_baseScale) {
                            const float bs = env->GetFloatField(rs, C.rs_baseScale);
                            const float as = C.rs_ageScale ? env->GetFloatField(rs, C.rs_ageScale) : 1.f;
                            rsScale = (bs > 0.f ? bs : 1.f) * (as > 0.f ? as : 1.f);
                        }
                        haveRS = true;
                    }
                    env->DeleteLocalRef(rs);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }


            // Live AABB translated by the interpolation offset (robust route).
            jobject bb = env->CallObjectMethod(ent, C.ent_getBox);
            if (!bb) break;
            const double dX = rx - cxr, dY = ry - cyr, dZ = rz - czr;
            double minX = env->GetDoubleField(bb, C.box_minX) + dX;
            double minY = env->GetDoubleField(bb, C.box_minY) + dY;
            double minZ = env->GetDoubleField(bb, C.box_minZ) + dZ;
            double maxX = env->GetDoubleField(bb, C.box_maxX) + dX;
            double maxY = env->GetDoubleField(bb, C.box_maxY) + dY;
            double maxZ = env->GetDoubleField(bb, C.box_maxZ) + dZ;

            // The 2D/3D boxes enclose the rendered shoulders and head,
            // which extend beyond the player's collision box. World-space
            // margins keep the coverage consistent with distance and perspective.
            if (g_settings.mode == 0 || g_settings.mode == 1) {
                const double lateral_margin = g_settings.mode == 1 ? 0.15 : 0.20;
                const double side_pad = (maxX - minX) * lateral_margin;
                const double depth_pad = (maxZ - minZ) * lateral_margin;
                const double head_pad = (maxY - minY) * 0.10;
                minX -= side_pad;
                maxX += side_pad;
                minZ -= depth_pad;
                maxZ += depth_pad;
                maxY += head_pad;
                // Extra room below the feet keeps the rounded 2D corners from
                // cutting into the boots; its health bar uses the same bounds.
                if (g_settings.mode == 0) minY -= (maxY - minY) * 0.025;
            }

            const double corners[8][3] = {
                {minX,minY,minZ},{maxX,minY,minZ},{minX,minY,maxZ},{maxX,minY,maxZ},
                {minX,maxY,minZ},{maxX,maxY,minZ},{minX,maxY,maxZ},{maxX,maxY,maxZ},
            };
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
            bool all = true; int nvis = 0;
            for (int i = 0; i < 8; ++i) {
                float px = 0.f, py = 0.f;
                if (!project(P, corners[i][0], corners[i][1], corners[i][2], px, py)) { all = false; continue; }
                t.cxp[i] = px; t.cyp[i] = py; ++nvis;
                bx0 = std::min(bx0, px); by0 = std::min(by0, py);
                bx1 = std::max(bx1, px); by1 = std::max(by1, py);
            }
            // Only give up when the box is entirely behind the camera. With a few
            // corners behind we still have a usable 2D extent; the 3D box just
            // stops drawing (corners_valid) so no edge shoots across the screen.
            if (nvis == 0) break;
            // Fully off-screen? skip.
            if (bx1 < 0 || by1 < 0 || bx0 > P.vw || by0 > P.vh) break;

            t.corners_valid = all;
            t.box_valid = true;
            t.minx = bx0; t.miny = by0; t.maxx = bx1; t.maxy = by1;

            const float hp  = env->CallFloatMethod(ent, C.liv_getHealth);
            const float mhp = env->CallFloatMethod(ent, C.liv_getMaxHealth);
            if (mhp > 0.f) { t.has_health = true; t.health_frac = std::clamp(hp / mhp, 0.f, 1.f); }

            // Wireframe / Skeleton: same animated body model; Wireframe draws the
            // part boxes, Skeleton draws lines between the joints.
            if ((g_settings.mode == 2 || g_settings.mode == 3) && C.liv_bodyYaw && C.liv_lastBodyYaw) {
                constexpr float DEG = 3.14159265f / 180.f;
                const float tpp = tp;   // forward-predicted progress (from the slider)
                auto lerp_ang = [&](float last, float cur) {
                    float d = cur - last; while (d < -180.f) d += 360.f; while (d > 180.f) d -= 360.f;
                    return last + d * tpp;
                };
                const float byaw = haveRS ? rsBodyYaw
                                          : lerp_ang(env->GetFloatField(ent, C.liv_lastBodyYaw),
                                                     env->GetFloatField(ent, C.liv_bodyYaw));
                const float yaw = byaw * DEG;
                const float ct = std::cos(yaw), st = std::sin(yaw);
                // MC draws the biped at a FIXED 0.9375 scale (times the render
                // state's base/age scale for babies). Deriving it from the hitbox
                // was wrong: the hitbox shrinks when sneaking/swimming while the
                // model does not, which made the whole wireframe shrink.
                const float sc = 0.9375f * rsScale;

                // Whole-model drop while crouching. Normally this arrives in the
                // render state's positionOffset; apply it here only if it did not.
                const float rootDy = (sneaking && !gotPosOffset) ? -0.125f * rsScale : 0.f;

                // Swim / crawl: the model tips toward horizontal about the feet.
                // MC rotates by lerp(lean, 0, -90 - pitch); our Z runs the other
                // way, so the angle is negated here. The swim translate is applied
                // before the rotation (matrix order R * T).
                float leanRad = 0.f, swimDy = 0.f, swimDz = 0.f;
                if (leanPitch > 0.001f) {
                    // MC: lean = -(90 + pitch), and the head part then adds +pitch
                    // so the two cancel and the head rides horizontal. MC applies
                    // the lean BEFORE its scale(-1,-1,1) and the head pitch after,
                    // so relative to each other they run opposite ways; in our
                    // single frame the head pitch is negated instead (below), and
                    // the lean keeps MC's magnitude so diving tilts the body more.
                    const float target = inWater ? (90.f + rsPitch) : 90.f;
                    leanRad = (leanPitch * target) * DEG;
                    if (swimPose) { swimDy = -1.0f; swimDz = -0.3f; }
                }
                const float cL = std::cos(leanRad), sL = std::sin(leanRad);

                // Walk-swing (biped formula) from the LimbAnimator.
                float la = rsLimbPos, amp = rsLimbAmp;
                if (!haveRS && C.liv_limbAnimator && C.la_getProgress && C.la_getAmplitude) {
                    jobject limb = env->GetObjectField(ent, C.liv_limbAnimator);
                    if (limb) {
                        la  = env->CallFloatMethod(limb, C.la_getProgress, tpp);
                        amp = env->CallFloatMethod(limb, C.la_getAmplitude, tpp);
                        env->DeleteLocalRef(limb);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                const float ld = std::clamp(amp, 0.f, 1.0f);
                const float cw = std::cos(la * 0.6662f);
                // Head look: pitch (tilt) + yaw relative to the body.
                float headPitch = 0.f, headRelYaw = 0.f;
                if (haveRS) {
                    headPitch  =  rsPitch   * DEG;
                    headRelYaw = -rsRelHead * DEG;   // X-mirror reverses the yaw sense
                }
                else if (C.ent_pitch && C.ent_lastPitch)
                    headPitch = lerp_ang(env->GetFloatField(ent, C.ent_lastPitch),
                                         env->GetFloatField(ent, C.ent_pitch)) * DEG;
                if (!haveRS && C.liv_headYaw && C.liv_lastHeadYaw) {
                    const float hyaw = lerp_ang(env->GetFloatField(ent, C.liv_lastHeadYaw),
                                                env->GetFloatField(ent, C.liv_headYaw));
                    float rel = hyaw - byaw; while (rel < -180.f) rel += 360.f; while (rel > 180.f) rel -= 360.f;
                    headRelYaw = -rel * DEG;   // X-mirror reverses the Y (yaw) sense
                }
                // Idle "breathing" arm sway (biped setAngles), ageInTicks = age+tp.
                const float ageT = (C.ent_age ? (float)env->GetIntField(ent, C.ent_age) : 0.f) + tpp;
                const float idleP = std::sin(ageT * 0.067f) * 0.05f;
                const float idleR = std::cos(ageT * 0.09f) * 0.05f + 0.05f;


                // Slim (Alex) arms?
                bool slim = false;
                if (C.acp_getPLE && C.ple_getSkin && C.skin_model && C.slim_obj) {
                    jobject ple = env->CallObjectMethod(ent, C.acp_getPLE);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (ple) {
                        jobject skn = env->CallObjectMethod(ple, C.ple_getSkin);
                        if (skn) {
                            jobject md = env->CallObjectMethod(skn, C.skin_model);
                            if (md) { slim = env->IsSameObject(md, C.slim_obj) == JNI_TRUE; env->DeleteLocalRef(md); }
                            env->DeleteLocalRef(skn);
                        }
                        env->DeleteLocalRef(ple);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }

                // Per-part rotations about the pivot (order Rx·Ry·Rz, ModelPart).
                int swingingArm = -1;   // set below when a hand swing is running
                float pit[6] = { headPitch, 0.f, -cw*ld + idleP, cw*ld - idleP, cw*1.4f*ld, -cw*1.4f*ld };
                float yw6[6] = { headRelYaw, 0.f, 0.f, 0.f, 0.f, 0.f };
                float rol[6] = { 0.f, 0.f, idleR, -idleR, 0.f, 0.f };

                // Main arm side (RIGHT -> part 2, LEFT -> part 3).
                bool isRightMain = true;
                if (C.liv_getMainArm && C.arm_right) {
                    jobject arm = env->CallObjectMethod(ent, C.liv_getMainArm);
                    if (arm) { isRightMain = env->IsSameObject(arm, C.arm_right) == JNI_TRUE; env->DeleteLocalRef(arm); }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                const int mainArm = isRightMain ? 2 : 3;
                const int offArm  = isRightMain ? 3 : 2;

                // Held-item pose: tilt the holding arm forward (ArmPose.ITEM).
                auto hasItem = [&](jmethodID getter) -> bool {
                    if (!getter || !C.stack_empty) return false;
                    jobject stk = env->CallObjectMethod(ent, getter);
                    bool has = false;
                    if (stk) { has = env->CallBooleanMethod(stk, C.stack_empty) != JNI_TRUE; env->DeleteLocalRef(stk); }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    return has;
                };
                if (sneaking) {
                    pit[1] += 0.5f;    // body lean
                    pit[2] += 0.4f;    // both arms follow the lean
                    pit[3] += 0.4f;
                }
                if (hasItem(C.liv_getMainStack)) { pit[mainArm] = pit[mainArm]*0.5f - 0.31416f; yw6[mainArm] = 0.f; }
                if (hasItem(C.liv_getOffStack))  { pit[offArm]  = pit[offArm]*0.5f  - 0.31416f; yw6[offArm]  = 0.f; }

                // Attack swing on the swinging arm (biped setAngles formula).
                if (C.liv_getSwing) {
                    const float sw = env->CallFloatMethod(ent, C.liv_getSwing, tpp);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (sw > 0.001f) {
                        bool swingMain = true;
                        if (C.liv_prefHand && C.hand_main) {
                            jobject ph = env->GetObjectField(ent, C.liv_prefHand);
                            if (ph) { swingMain = env->IsSameObject(ph, C.hand_main) == JNI_TRUE; env->DeleteLocalRef(ph); }
                        }
                        const int swArm = (swingMain == isRightMain) ? 2 : 3;
                        swingingArm = swArm;
                        float f = 1.f - sw; f *= f; f *= f; f = 1.f - f;
                        const float g = std::sin(f * 3.14159265f);
                        const float hh = std::sin(sw * 3.14159265f) * -(headPitch - 0.7f) * 0.75f;
                        pit[swArm] -= g * 1.2f + hh;
                        rol[swArm] += std::sin(sw * 3.14159265f) * -0.4f;
                    }
                }

                // ── Swim / crawl stroke (BipedEntityModel, 26-tick cycle) ──
                // Our space mirrors X, which negates yaw and roll but leaves
                // pitch alone, so MC's yaw/roll targets are negated here.
                if (leanPitch > 0.001f) {
                    const float w = std::clamp(leanPitch, 0.f, 1.f);
                    // Blend the head's pitch from +pitch (upright) to -pitch (fully
                    // leaned) so it cancels against the lean and rides horizontal.
                    pit[0] = headPitch * (1.f - 2.f * w);
                    const float g26 = std::fmod(std::fabs(la), 26.f);
                    auto ee = [](float x) { return -65.f * x + x * x; };
                    auto lf = [](float d, float a, float b) { return a + (b - a) * d; };
                    constexpr float PI_ = 3.14159265f;
                    float rP, rY, rR, lP, lY, lR;
                    if (g26 < 14.f) {
                        lP = 0.f;  rP = 0.f;
                        lY = PI_;  rY = PI_;
                        lR = PI_ + 1.8707964f * ee(g26) / ee(14.f);
                        rR = PI_ - 1.8707964f * ee(g26) / ee(14.f);
                    } else if (g26 < 22.f) {
                        const float j = (g26 - 14.f) / 8.f;
                        lP = (PI_ * 0.5f) * j;  rP = lP;
                        lY = PI_;  rY = PI_;
                        lR = 5.012389f  - 1.8707964f * j;
                        rR = 1.2707963f + 1.8707964f * j;
                    } else {
                        const float j = (g26 - 22.f) / 4.f;
                        lP = (PI_ * 0.5f) * (1.f - j);  rP = lP;
                        lY = PI_;  rY = PI_;
                        lR = PI_;  rR = PI_;
                    }
                    // MC blends the stroke in with weight = leaningPitch, except on
                    // the arm that is mid-swing, which keeps its attack animation.
                    const float wR = (swingingArm == 2) ? 0.f : w;
                    const float wL = (swingingArm == 3) ? 0.f : w;
                    pit[2] = lf(wR, pit[2],  rP);  yw6[2] = lf(wR, yw6[2], -rY);  rol[2] = lf(wR, rol[2], rR);
                    pit[3] = lf(wL, pit[3],  lP);  yw6[3] = lf(wL, yw6[3], -lY);  rol[3] = lf(wL, rol[3], lR);
                    // Legs flutter — applies even while holding an item.
                    pit[4] = lf(w, pit[4], 0.3f * std::cos(la * 0.33333334f));
                    pit[5] = lf(w, pit[5], 0.3f * std::cos(la * 0.33333334f + PI_));
                }

                // ── Riding: legs splay out to the sides, arms lift ──
                if (riding) {
                    pit[2] += -0.62831855f;
                    pit[3] += -0.62831855f;
                    pit[4] = -1.4137167f; yw6[4] = -0.31415927f; rol[4] =  0.07853982f;
                    pit[5] = -1.4137167f; yw6[5] =  0.31415927f; rol[5] = -0.07853982f;
                }

                if (leanPitch > 0.01f) {
                    constexpr float R2D = 57.29578f;
                    dbg::log_rl(20, 400,
                        "swim: lean=%.3f swimPose=%d water=%d rsPitch=%.1f relHead=%.1f la=%.2f g26=%.2f leanDeg=%.1f",
                        leanPitch, (int)swimPose, (int)inWater, rsPitch, rsRelHead,
                        la, std::fmod(std::fabs(la), 26.f), leanRad * R2D);
                    dbg::log_rl(21, 400,
                        "swim-parts: head(p=%.1f y=%.1f r=%.1f) rArm(p=%.1f y=%.1f r=%.1f) lArm(p=%.1f y=%.1f r=%.1f)"
                        " rLeg(p=%.1f) lLeg(p=%.1f) body(p=%.1f)",
                        pit[0]*R2D, yw6[0]*R2D, rol[0]*R2D,
                        pit[2]*R2D, yw6[2]*R2D, rol[2]*R2D,
                        pit[3]*R2D, yw6[3]*R2D, rol[3]*R2D,
                        pit[4]*R2D, pit[5]*R2D, pit[1]*R2D);
                }

                // Shared point transform: local (lx,ly,lz) of part pi -> screen.
                auto xform = [&](int pi, float pvx, float pvy, float pvz, float lx, float ly, float lz,
                                 float& ox, float& oy) -> bool {
                    const float cP = std::cos(pit[pi]), sP = std::sin(pit[pi]);
                    const float cY = std::cos(yw6[pi]), sY = std::sin(yw6[pi]);
                    const float cR = std::cos(rol[pi]), sR = std::sin(rol[pi]);
                    const float dx = lx - pvx, dy = ly - pvy, dz = lz - pvz;
                    float ax = dx, ay = dy*cP - dz*sP, az = dy*sP + dz*cP;   // Rx
                    float bx = ax*cY - az*sY, by2 = ay, bz = ax*sY + az*cY;  // Ry
                    float exx = bx*cR - by2*sR, ey = bx*sR + by2*cR, ez = bz; // Rz
                    const float mlx = -(pvx + exx) * sc;   // MC scale(-1) X-mirror
                    float mly = (pvy + ey) * sc;
                    float mlz = (pvz + ez) * sc;
                    if (leanRad != 0.f) {          // swim / crawl lean about the feet
                        const float ty = mly + swimDy, tz = mlz + swimDz;
                        mly = ty * cL - tz * sL;
                        mlz = ty * sL + tz * cL;
                    }
                    mly += rootDy;
                    return project(P, rx + (mlx*ct - mlz*st), ry + mly, rz + (mlx*st + mlz*ct), ox, oy);
                };

                if (g_settings.mode == 2) {   // Wireframe: box corners per part.
                    for (int pi = 0; pi < 6; ++pi) {
                        float x0 = WIRE_PARTS[pi][0], y0 = WIRE_PARTS[pi][1], z0 = WIRE_PARTS[pi][2];
                        float x1 = WIRE_PARTS[pi][3], y1 = WIRE_PARTS[pi][4], z1 = WIRE_PARTS[pi][5];
                        float pvx = WIRE_PIVOT[pi][0], pvy = WIRE_PIVOT[pi][1], pvz = 0.f;
                        if (slim && pi == 2) { x1 = 0.4375f;  pvx = 0.28125f; }   // 4.5/16
                        if (slim && pi == 3) { x0 = -0.4375f; pvx = -0.28125f; }
                        if (sneaking) {
                            const float dy = SNEAK_DY[pi], dz = SNEAK_DZ[pi];
                            y0 += dy; y1 += dy; pvy += dy;
                            z0 += dz; z1 += dz; pvz += dz;
                        }
                        const float xs[2] = { x0, x1 }, ys[2] = { y0, y1 }, zs[2] = { z0, z1 };
                        bool pall = true; int ci = 0; (void)pall;
                        for (int xi = 0; xi < 2; ++xi)
                            for (int yi = 0; yi < 2; ++yi)
                                for (int zi = 0; zi < 2; ++zi) {
                                    float px = 0.f, py = 0.f;
                                    const bool okc = xform(pi, pvx, pvy, pvz, xs[xi], ys[yi], zs[zi], px, py);
                                    if (!okc) pall = false;
                                    t.wok[pi][ci] = okc;
                                    t.wxp[pi][ci] = px; t.wyp[pi][ci] = py; ++ci;
                                }
                        // A part is kept when at least one corner projected; edges
                        // with an endpoint behind the camera are skipped when drawing,
                        // so a limb crossing the near plane no longer vanishes whole.
                        t.wpart[pi] = false;
                        int nok = 0;
                        for (int c2 = 0; c2 < 8; ++c2) if (t.wok[pi][c2]) { t.wpart[pi] = true; ++nok; }
                        if (leanPitch > 0.01f)
                            dbg::log_rl(22 + (pi > 5 ? 5 : pi), 400,
                                "swim-proj: part%d corners_ok=%d/8 first=(%.0f,%.0f)",
                                pi, nok, t.wxp[pi][0], t.wyp[pi][0]);
                    }
                } else {                      // Skeleton: joint points, same animation.
                    auto J = [&](int pi, float lx, float ly, float lz, int idx) {
                        const float sdy = sneaking ? SNEAK_DY[pi] : 0.f;
                        const float sdz = sneaking ? SNEAK_DZ[pi] : 0.f;
                        t.sjok[idx] = xform(pi, WIRE_PIVOT[pi][0], WIRE_PIVOT[pi][1] + sdy, sdz,
                                            lx, ly + sdy, lz + sdz, t.sjx[idx], t.sjy[idx]);
                    };
                    J(0,  0.000f, 1.75f, 0.f, 0);  // head node ~ middle of the head
                    J(1,  0.000f, 1.50f, 0.f, 1);  // neck
                    J(1,  0.000f, 0.75f, 0.f, 2);  // hip centre
                    J(2,  0.375f, 1.50f, 0.f, 3);  // right shoulder
                    J(2,  0.375f, 0.75f, 0.f, 4);  // right hand
                    J(3, -0.375f, 1.50f, 0.f, 5);  // left shoulder
                    J(3, -0.375f, 0.75f, 0.f, 6);  // left hand
                    J(4,  0.125f, 0.00f, 0.f, 7);  // right foot
                    J(5, -0.125f, 0.00f, 0.f, 8);  // left foot
                    J(4,  0.125f, 0.75f, 0.f, 9);  // right hip
                    J(5, -0.125f, 0.75f, 0.f, 10); // left hip
                    t.skel = true;
                }
            }

            if (want_equip) {
                const jobject slots[4] = { E.slot_head, E.slot_chest, E.slot_legs, E.slot_feet };
                for (int i = 0; i < 4; ++i)
                    t.equip[i] = resolve_icon(env, ent, slots[i], itemsAtlas, blockAtlas,
                                              missItems, missBlock, itemsGl, blockGl);
            }
            keep = true;
        } while (false);

        env->PopLocalFrame(nullptr);
        if (keep) out.push_back(t);
        env->DeleteLocalRef(ent);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    g_items_gl_cur = itemsGl; g_block_gl_cur = blockGl;

    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);   // frees mc/world/self/iterator/atlas refs

    std::lock_guard<std::mutex> lk(g_mx);
    g_targets.swap(out);
}

// ── Drawing ──────────────────────────────────────────────────────────────────
// The reference is the projected box silhouette, not its internal edges.
// A convex hull naturally becomes a rectangle or hexagon as the camera turns.
int box_silhouette(const EspTarget& target, ImVec2* hull)
{
    ImVec2 points[8];
    for (int i = 0; i < 8; ++i) points[i] = ImVec2(target.cxp[i], target.cyp[i]);
    std::sort(points, points + 8, [](const ImVec2& a, const ImVec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    auto cross = [](const ImVec2& a, const ImVec2& b, const ImVec2& c) {
        return (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x);
    };
    int n = 0;
    for (const ImVec2& point : points) {
        while (n >= 2 && cross(hull[n-2], hull[n-1], point) <= 0.f) --n;
        hull[n++] = point;
    }
    const int lower = n + 1;
    for (int i = 6; i >= 0; --i) {
        while (n >= lower && cross(hull[n-2], hull[n-1], points[i]) <= 0.f) --n;
        hull[n++] = points[i];
    }
    return n > 1 ? n - 1 : n;
}

// Adjacent halo segments share vertices. No overlapping line caps or stacked
// strokes at the corners, which previously made bright knots and flicker.
void silhouette_glow(ImDrawList* dl, const ImVec2* hull, int count, float thickness)
{
    ImVec2 outward[16];
    for (int i = 0; i < count; ++i) {
        const ImVec2& prev = hull[(i + count - 1) % count];
        const ImVec2& curr = hull[i];
        const ImVec2& next = hull[(i + 1) % count];
        const float ax = curr.x - prev.x, ay = curr.y - prev.y;
        const float bx = next.x - curr.x, by = next.y - curr.y;
        const float al = std::max(0.001f, std::sqrt(ax*ax + ay*ay));
        const float bl = std::max(0.001f, std::sqrt(bx*bx + by*by));
        const ImVec2 na(ay/al, -ax/al), nb(by/bl, -bx/bl);
        const float denom = std::max(0.5f, 1.f + na.x*nb.x + na.y*nb.y);
        outward[i] = ImVec2((na.x+nb.x)/denom, (na.y+nb.y)/denom);
    }
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    // One continuous mesh from the inner AA edge through the core and halo.
    // Thickness changes only the core, leaving the glow's reach and opacity fixed.
    const float half = thickness * 0.5f;
    const float distances[] = {-half-0.5f, -half, half, half+0.6f, half+1.2f,
        half+1.8f, half+2.4f, half+3.0f, half+3.6f, half+4.2f, half+4.8f};
    const float alpha[] = {0.f, 0.92f, 0.92f, 0.20f, 0.14f, 0.095f,
        0.060f, 0.035f, 0.018f, 0.007f, 0.f};
    for (int ring = 0; ring < 10; ++ring) {
        const float d0 = distances[ring], d1 = distances[ring+1];
        const ImU32 c0 = pack_rgba(g_settings.col_r, g_settings.col_g, g_settings.col_b,
            g_settings.col_a * alpha[ring]);
        const ImU32 c1 = pack_rgba(g_settings.col_r, g_settings.col_g, g_settings.col_b,
            g_settings.col_a * alpha[ring+1]);
        dl->PrimReserve(count * 6, count * 2);
        const unsigned int base = dl->_VtxCurrentIdx;
        for (int i = 0; i < count; ++i) {
            dl->PrimWriteVtx(ImVec2(hull[i].x + outward[i].x*d0, hull[i].y + outward[i].y*d0), uv, c0);
            dl->PrimWriteVtx(ImVec2(hull[i].x + outward[i].x*d1, hull[i].y + outward[i].y*d1), uv, c1);
        }
        for (int i = 0; i < count; ++i) {
            const unsigned int a = base + i*2, b = base + ((i+1)%count)*2;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b+1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b+1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a+1));
        }
    }
}

void draw(int display_w, int display_h)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    std::lock_guard<std::mutex> lk(g_mx);
    if (g_targets.empty()) return;

    static float outline_width = 1.0f;
    const float target_width = std::clamp(g_settings.thickness, 0.75f, 3.f);
    outline_width += (target_width - outline_width)
        * (1.f - std::exp(-18.f * std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f)));

    const ImU32 col   = pack_rgba(g_settings.col_r, g_settings.col_g, g_settings.col_b, g_settings.col_a);
    const ImU32 black = pack_rgba(0.f, 0.f, 0.f, g_settings.col_a);

    auto line2 = [&](float x0, float y0, float x1, float y1, float wDark, float wCol) {
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), black, wDark);
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col,   wCol);
    };

    dl->PushClipRect(ImVec2(0.f, 0.f), ImVec2((float)display_w, (float)display_h), false);

    // Crisp, mip-free sampling for the equipment atlas across all icon draws.
    const bool want_icons = g_settings.equipment && (g_items_gl_cur || g_block_gl_cur);
    if (want_icons) {
        g_fsave.n = 0;
        if (g_items_gl_cur) g_fsave.gl[g_fsave.n++] = g_items_gl_cur;
        if (g_block_gl_cur) g_fsave.gl[g_fsave.n++] = g_block_gl_cur;
        dl->AddCallback(cb_set_nearest, &g_fsave);
    }

    for (const EspTarget& t : g_targets) {
        if (!t.box_valid) continue;
        const float w = t.maxx - t.minx, h = t.maxy - t.miny;
        if (w < 1.f || h < 1.f) continue;

        // ── ESP body: one of 2D box / 3D box / wireframe (mutually exclusive) ──
        // Fill (if on) is drawn first, bounded exactly by the projected corners
        // so it never spills past the outline.
        const ImU32 fillc = pack_rgba(g_settings.col_r, g_settings.col_g, g_settings.col_b,
                                      g_settings.col_a * 0.20f);
        if (g_settings.mode == 1) {
            if (t.corners_valid) {
                ImVec2 hull[16];
                const int count = box_silhouette(t, hull);
                if (count >= 3) {
                    if (g_settings.fill) dl->AddConvexPolyFilled(hull, count, fillc);
                    silhouette_glow(dl, hull, count, outline_width);
                }
            }
        }
        else if (g_settings.mode == 2) {
            // Wireframe: per body-part box.
            for (int pi = 0; pi < 6; ++pi) {
                if (!t.wpart[pi]) continue;
                if (g_settings.fill)
                    for (auto& f : BOX_FACES) {
                        if (!t.wok[pi][f[0]] || !t.wok[pi][f[1]] || !t.wok[pi][f[2]] || !t.wok[pi][f[3]]) continue;
                        dl->AddQuadFilled(ImVec2(t.wxp[pi][f[0]],t.wyp[pi][f[0]]), ImVec2(t.wxp[pi][f[1]],t.wyp[pi][f[1]]),
                                          ImVec2(t.wxp[pi][f[2]],t.wyp[pi][f[2]]), ImVec2(t.wxp[pi][f[3]],t.wyp[pi][f[3]]), fillc);
                    }
                for (auto& e : WIRE_EDGES) {
                    if (!t.wok[pi][e[0]] || !t.wok[pi][e[1]]) continue;
                    line2(t.wxp[pi][e[0]], t.wyp[pi][e[0]],
                          t.wxp[pi][e[1]], t.wyp[pi][e[1]], 1.4f, 0.9f);
                }
            }
        }
        else if (g_settings.mode == 3) {
            // Skeleton: joint-to-joint lines (no fill). head, spine, collars,
            // arms, hips, and PARALLEL legs (hip -> foot on each side).
            static const int SEG[10][2] = {
                {1,0},{1,2},{1,3},{3,4},{1,5},{5,6},{2,9},{2,10},{9,7},{10,8}
            };
            for (auto& s : SEG)
                if (t.sjok[s[0]] && t.sjok[s[1]])
                    line2(t.sjx[s[0]], t.sjy[s[0]], t.sjx[s[1]], t.sjy[s[1]], 2.4f, 1.3f);
        }
        else {
            const ImVec2 p0(t.minx, t.miny), p1(t.maxx, t.maxy);
            const float rounding = std::min(w * 0.16f, h * 0.09f);
            if (g_settings.fill) {
                dl->AddRectFilled(p0, p1, IM_COL32(8, 17, 23, 100), rounding);
                dl->AddRectFilled(p0, p1, fillc, rounding);
            }
            for (int pass = 3; pass > 0; --pass)
                dl->AddRect(p0, p1, pack_rgba(g_settings.col_r, g_settings.col_g,
                    g_settings.col_b, g_settings.col_a * 0.025f * (4 - pass)),
                    rounding, 0, outline_width + pass * 1.25f);
            dl->AddRect(p0, p1, pack_rgba(g_settings.col_r, g_settings.col_g,
                g_settings.col_b, g_settings.col_a * 0.65f), rounding, 0, outline_width);
        }

        if (g_settings.health && t.has_health) {
            if (g_settings.mode >= 2) {
                // Preserve the original health rendering for Wireframe and Skeleton.
                const float x1 = t.minx - 3.f, x0 = x1 - 3.f;
                dl->AddRectFilled(ImVec2(x0-1, t.miny-1), ImVec2(x1+1, t.maxy+1), IM_COL32(0,0,0,180));
                dl->AddRectFilled(ImVec2(x0, t.maxy - h * t.health_frac), ImVec2(x1, t.maxy),
                    pack_rgba(1.f - t.health_frac, t.health_frac, 0.f, 1.f));
            } else {
                const float bw = std::clamp(w * 0.035f, 3.5f, 7.f);
                const float x1 = t.minx - std::max(5.f, bw), x0 = x1 - bw;
                dl->AddRectFilled(ImVec2(x0-1.f, t.miny-1.f), ImVec2(x1+1.f, t.maxy+1.f),
                    IM_COL32(9,12,15,190), bw * 0.5f + 1.f);
                const float stops[4][3] = {{1.f,0.12f,0.12f}, {1.f,0.42f,0.04f},
                    {1.f,0.86f,0.08f}, {0.12f,0.95f,0.20f}};
                const float f = std::clamp(t.health_frac, 0.f, 1.f) * 3.f;
                const int k = std::min(2, static_cast<int>(f));
                const float blend = f - k;
                const float r = stops[k][0] + (stops[k+1][0] - stops[k][0]) * blend;
                const float g = stops[k][1] + (stops[k+1][1] - stops[k][1]) * blend;
                const float b = stops[k][2] + (stops[k+1][2] - stops[k][2]) * blend;
                if (t.health_frac > 0.f) {
                    const float top = t.maxy - h * t.health_frac;
                    for (int pass = 3; pass > 0; --pass) {
                        const float pad = pass * 0.8f;
                        dl->AddRectFilled(ImVec2(x0-pad, top-pad), ImVec2(x1+pad, t.maxy+pad),
                            pack_rgba(r,g,b,0.035f * (4-pass)), bw * 0.5f + pad);
                    }
                    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, t.maxy),
                        pack_rgba(r,g,b,1.f), bw * 0.5f);
                }
            }
        }

        // Equipment icons: a FIXED on-screen size (distance-independent), stacked
        // and vertically centred beside the box — same size and aligned whether
        // the player is near or far.
        if (g_settings.equipment) {
            // Scale with the player so the icons stay PROPORTIONAL to the box at
            // every distance (the four pieces ~= the box height); centred beside
            // it. A low floor keeps the stack ~= the box height even when far, so
            // it never overflows a tiny box and drifts off ("descolocado").
            const float sz  = std::clamp(h / 4.5f, 3.f, 80.f);
            const float gap = sz * 0.12f;
            int n = 0;
            for (int i = 0; i < 4; ++i) if (t.equip[i].present) ++n;
            if (n > 0) {
                const float x0 = t.maxx + sz * 0.16f;
                const float totalH = n * sz + (n - 1) * gap;
                float y = (t.miny + t.maxy) * 0.5f - totalH * 0.5f;
                for (int i = 0; i < 4; ++i) {
                    const EquipIcon& ic = t.equip[i];
                    if (!ic.present) continue;
                    const ImVec2 p0(x0, y), p1(x0 + sz, y + sz);
                    if (ic.sprite && ic.gl_id) {
                        dl->AddImage(static_cast<ImTextureID>(ic.gl_id), p0, p1,
                                     ImVec2(ic.u0, ic.v0), ImVec2(ic.u1, ic.v1));
                    } else {
                        dl->AddRectFilled(p0, p1, ic.tint);
                        dl->AddRect(p0, p1, IM_COL32(0,0,0,200));
                        auto& fb = arraylist::shared_pool();
                        if (fb.ensure()) {
                            ImFont* fnt = fb.get(sz * 0.9f);
                            if (fnt) {
                                char s[2] = { ic.letter, 0 };
                                const ImVec2 ts = fnt->CalcTextSizeA(sz*0.9f, FLT_MAX, -1.f, s);
                                dl->PushTextureID(fb.texture_id());
                                dl->AddText(fnt, sz*0.9f,
                                            ImVec2(x0 + (sz - ts.x)*0.5f, y + (sz - ts.y)*0.5f),
                                            IM_COL32(20,20,20,255), s);
                                dl->PopTextureID();
                            }
                        }
                    }
                    y += sz + gap;
                }
            }
        }
    }

    if (want_icons) {
        dl->AddCallback(cb_restore_filter, &g_fsave);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    dl->PopClipRect();

}

} // namespace

void install()
{
    // The capture service is shared with hitfx; installing is idempotent.
    cap::install();
    VLOG_INFO("esp", "install() done (capture installed=%d)", (int)cap::is_installed());
}

void shutdown()
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_targets.clear();
}

void render_text(int display_w, int display_h)
{
    if (g_settings_provider) g_settings_provider(g_settings);

    if (!g_enabled || display_w < 2 || display_h < 2) {
        std::lock_guard<std::mutex> lk(g_mx); g_targets.clear();
        return;
    }

    // Don't draw the ESP over the ClickGui panel while it's open.
    if (::vanta::vgui::bridge::isOverlayVisible()) {
        std::lock_guard<std::mutex> lk(g_mx); g_targets.clear();
        return;
    }

    // Retry capture install (rate-limited) until the world is loaded.
    if (!cap::is_installed()) {
        static double s_next = 0.0;
        const double now = ImGui::GetTime();
        if (now >= s_next) { s_next = now + 1.0; cap::install(); }
    }
    cap::set_capture_enabled(true);

    {
        vanta::jvm::JniScope s;
        if (s) {
            cap::capture_now(s.get());
            if (init_core(s.get())) {
                hf::render_frame cam{};
                if (hf::latest_frame(cam)) poll(s.get(), cam);
            }
        }
    }

    draw(display_w, display_h);
}

} // namespace vanta::modules::esp
