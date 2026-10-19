#include "core/NativeCallbacks.h"
// AntiAction -- stops the handful of right-clicks and left-clicks that lose a
// fight: filling an anchor that is already charged, opening an ender chest with
// a weapon in hand, dropping glowstone on the floor instead of into an anchor,
// and swinging a sword into a raised shield.
//
// Every rule is evaluated AT the action, on the game thread, against the same
// state vanilla is about to act on: MinecraftClient.crosshairTarget, the main
// hand stack, the block under the crosshair. Nothing is predicted a frame early
// and nothing is remembered between clicks, so there is no way for the gate to
// drift from what the game would actually have done.
//
// Two action sites:
//
//   left click  -- the triggerbot already owns a class_310.doAttack detour. It
//                  asks should_block_attack() before calling the original and
//                  returns JNI_FALSE when told to: no attack, no swing, no
//                  packet. See triggerbot_hook.cpp.
//
//   right click -- this file installs its own detour on class_310.doItemUse
//                  (method_1583, private, ()V). It is the single funnel every
//                  use goes through -- entity, block and item, main and off
//                  hand -- and it runs BEFORE the interaction packet exists, so
//                  a blocked use produces no traffic at all. The server sees a
//                  player who did not click. The copy-class audit passes for it
//                  on 1.21.4 and 1.21.11 (tools/jnihook_copy_audit.py).
//
//                  When that hook cannot be installed the module falls back to
//                  the window: the frame pump evaluates the same predicate and,
//                  while it holds, raises the level-1 input filter so the
//                  right-click never reaches GLFW. Coarser (the block is a
//                  frame old) but the rules stay identical.
//
//                  The same detour carries one borrowed seam: a use observer
//                  (antiaction.h) that is asked before the rules and may
//                  consume the click outright. Safe Anchor registers it to
//                  take over the player's own click on an anchor. While an
//                  observer is registered the detour is installed even when
//                  this module is off; the rules still only run while it is on.
//
// Wire impact: none beyond absence. A blocked action sends nothing, and an
// allowed action is the untouched original. There is no packet for GrimAC or
// Vulcan to look at.
//
// Every intermediary name and descriptor below was checked against
// tools/genmaps/.cache/{1.21,1.21.11}/yarn_merged.tiny; none differ.

#include "antiaction.h"

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <sdk/mappings/mappings.hpp>
#include <jni.h>
#include <jnihook.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace vanta::modules {

namespace {

// ─── JNI cache ───────────────────────────────────────────────────────────────

struct Cache {
    bool tried = false;
    bool ready = false;

    jclass   mc_class          = nullptr;   // class_310  MinecraftClient
    jclass   entity_hit_class  = nullptr;   // class_3966 EntityHitResult
    jclass   block_hit_class   = nullptr;   // class_3965 BlockHitResult
    jclass   living_class      = nullptr;   // class_1309 LivingEntity
    jclass   integer_class     = nullptr;   // java/lang/Integer

    jfieldID player_fid        = nullptr;   // class_310.field_1724 : class_746
    jfieldID world_fid         = nullptr;   // class_310.field_1687 : class_638
    jfieldID crosshair_fid     = nullptr;   // class_310.field_1765 : class_239
    jfieldID use_cooldown_fid  = nullptr;   // class_310.field_1752 : I

    jmethodID get_entity_mid   = nullptr;   // class_3966.method_17782 ()class_1297
    jmethodID get_block_pos_mid= nullptr;   // class_3965.method_17777 ()class_2338
    jmethodID is_blocking_mid  = nullptr;   // class_1309.method_6039  ()Z
    jmethodID main_hand_mid    = nullptr;   // class_1309.method_6047  ()class_1799
    jmethodID is_sneaking_mid  = nullptr;   // class_1297.method_5715  ()Z
    jmethodID get_item_mid     = nullptr;   // class_1799.method_7909  ()class_1792
    jmethodID translation_mid  = nullptr;   // class_1792.method_7876  ()String
    jmethodID get_block_state_mid = nullptr;// class_1937.method_8320  (class_2338)class_2680
    jmethodID get_block_mid    = nullptr;   // class_2680.method_26204 ()class_2248
    jmethodID state_get_mid    = nullptr;   // class_2688.method_11654 (class_2769)Comparable
    jmethodID int_value_mid    = nullptr;   // Integer.intValue ()I
    jmethodID pos_x_mid        = nullptr;   // class_2338.method_10263 ()I
    jmethodID pos_y_mid        = nullptr;   // class_2338.method_10264 ()I
    jmethodID pos_z_mid        = nullptr;   // class_2338.method_10260 ()I

    jobject  respawn_anchor_ref = nullptr;  // class_2246.field_23152
    jobject  ender_chest_ref    = nullptr;  // class_2246.field_10443
    jobject  charges_ref        = nullptr;  // class_4969.field_23153 (IntProperty)
};

Cache g_c;

// Which rules are on. Mirrored from the settings by the module every frame so
// the hooks read plain atomics instead of walking Setting objects.
std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_rule_double_glowstone{ true };
std::atomic<bool> g_rule_open_echest{ true };
std::atomic<bool> g_rule_place_glowstone{ true };
std::atomic<bool> g_rule_sword_shield{ true };

// Raised before any detach so a detour already inside its body passes straight
// through to the original instead of touching a cache that is going away.
std::atomic<bool> g_shutting_down{ false };

// The use observer (see antiaction.h). Two atomics rather than a std::function
// under a mutex: the detour reads them on every right-click and a registration
// can arrive from the keybind thread. `user` is read after `fn`, and a clear
// writes `fn` first, so a stale pair is never handed to a live callback.
std::atomic<antiaction::UseObserver> g_use_observer{ nullptr };
std::atomic<void*>                   g_use_observer_user{ nullptr };

// ─── the doItemUse hook ──────────────────────────────────────────────────────

enum class HookState : int { Idle = 0, Live = 1, Failed = 2 };
std::atomic<int> g_hook_state{ static_cast<int>(HookState::Idle) };

jmethodID  g_do_item_use_mid = nullptr;
jmethodID  ORIG_doItemUse    = nullptr;

// The probe. doItemUse sets itemUseCooldown to 4 on every call that is not
// swallowed by "currently breaking a block", and handleInputEvents only calls
// it while the cooldown is 0, so a working original moves the field from 0 to
// 4 across the call. If the original has run several times and the field never
// moved, the copy class is a no-op (the public-override trap, or a mixin that
// ate the body) and the hook is detached rather than trusted -- an unverified
// hook here would silently kill every right-click.
constexpr int kProbeCalls = 5;
std::atomic<bool> g_probe_passed{ false };
std::atomic<bool> g_probe_failed{ false };
std::atomic<int>  g_probe_calls{ 0 };

// ─── last-blocked reason, for displayInfo and the rate-limited log ───────────

std::mutex           g_reason_mutex;
char                 g_reason[96] = {};
unsigned long long   g_reason_ms  = 0;
unsigned long long   g_last_log_ms = 0;
constexpr unsigned long long kReasonShowMs = 2000;
constexpr unsigned long long kLogMinGapMs  = 200;   // 5 lines/s

void note_blocked(const char* reason)
{
    const unsigned long long now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_reason_mutex);
    std::strncpy(g_reason, reason, sizeof(g_reason) - 1);
    g_reason[sizeof(g_reason) - 1] = '\0';
    g_reason_ms = now;
    if (now - g_last_log_ms >= kLogMinGapMs) {
        g_last_log_ms = now;
        VLOG_INFO("antiaction", "blocked: {}", reason);
    }
}

// ─── small JNI helpers ───────────────────────────────────────────────────────

// Clears a pending exception and reports whether one was there. Every JNI call
// in the gates goes through this: the gates run inside vanilla's own input
// handling and must never let a Java exception escape into it.
bool clear_exc(JNIEnv* env)
{
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    return false;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Translation key of the main-hand item ("item.minecraft.diamond_sword",
// "block.minecraft.glowstone"). Empty on any failure, which every rule reads
// as "not the item I care about".
std::string main_hand_key(JNIEnv* env, jobject player)
{
    std::string out;
    jobject stack = env->CallObjectMethod(player, g_c.main_hand_mid);
    if (clear_exc(env) || !stack) return out;
    jobject item = env->CallObjectMethod(stack, g_c.get_item_mid);
    env->DeleteLocalRef(stack);
    if (clear_exc(env) || !item) return out;
    jstring key = static_cast<jstring>(env->CallObjectMethod(item, g_c.translation_mid));
    env->DeleteLocalRef(item);
    if (clear_exc(env) || !key) return out;
    if (const char* utf = env->GetStringUTFChars(key, nullptr)) {
        out.assign(utf);
        env->ReleaseStringUTFChars(key, utf);
    }
    env->DeleteLocalRef(key);
    return out;
}

bool is_glowstone(std::string_view key) { return ends_with(key, ".glowstone"); }
bool is_sword(std::string_view key)     { return ends_with(key, "_sword"); }

// What counts as "holding combat items" for the ender chest rule: anything a
// crystal / anchor fighter cycles through, so a mis-click on the chest that is
// part of the arena never opens it mid-trade.
bool is_combat_item(std::string_view key)
{
    static constexpr std::string_view kSuffixes[] = {
        "_sword", "_axe", ".mace", ".trident", ".end_crystal", ".respawn_anchor",
        ".glowstone", ".obsidian", ".crying_obsidian", ".totem_of_undying",
    };
    for (auto s : kSuffixes) if (ends_with(key, s)) return true;
    return false;
}

bool player_sneaking(JNIEnv* env, jobject player)
{
    const jboolean v = env->CallBooleanMethod(player, g_c.is_sneaking_mid);
    return !clear_exc(env) && v == JNI_TRUE;
}

// Block under a BlockHitResult: fills `pos_out` (local ref, caller frees) and
// returns the Block (local ref) or null. A miss still comes through here as a
// BlockHitResult whose pos is wherever the ray ended -- that resolves to air,
// which is exactly "not an anchor" for the rules below.
jobject block_under(JNIEnv* env, jobject mc, jobject hit, jobject& pos_out, jobject& state_out)
{
    pos_out = nullptr;
    state_out = nullptr;
    jobject pos = env->CallObjectMethod(hit, g_c.get_block_pos_mid);
    if (clear_exc(env) || !pos) return nullptr;
    jobject world = env->GetObjectField(mc, g_c.world_fid);
    if (clear_exc(env) || !world) { env->DeleteLocalRef(pos); return nullptr; }
    jobject state = env->CallObjectMethod(world, g_c.get_block_state_mid, pos);
    env->DeleteLocalRef(world);
    if (clear_exc(env) || !state) { env->DeleteLocalRef(pos); return nullptr; }
    jobject block = env->CallObjectMethod(state, g_c.get_block_mid);
    if (clear_exc(env) || !block) {
        env->DeleteLocalRef(state);
        env->DeleteLocalRef(pos);
        return nullptr;
    }
    pos_out = pos;
    state_out = state;
    return block;
}

// RespawnAnchorBlock.CHARGES of a state, or -1.
int anchor_charges(JNIEnv* env, jobject state)
{
    jobject boxed = env->CallObjectMethod(state, g_c.state_get_mid, g_c.charges_ref);
    if (clear_exc(env) || !boxed) return -1;
    int v = -1;
    if (env->IsInstanceOf(boxed, g_c.integer_class) == JNI_TRUE) {
        v = env->CallIntMethod(boxed, g_c.int_value_mid);
        if (clear_exc(env)) v = -1;
    }
    env->DeleteLocalRef(boxed);
    return v;
}

void format_pos(JNIEnv* env, jobject pos, char* out, size_t n)
{
    const jint x = env->CallIntMethod(pos, g_c.pos_x_mid); clear_exc(env);
    const jint y = env->CallIntMethod(pos, g_c.pos_y_mid); clear_exc(env);
    const jint z = env->CallIntMethod(pos, g_c.pos_z_mid); clear_exc(env);
    std::snprintf(out, n, "%d %d %d", static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
}

// ─── cache ───────────────────────────────────────────────────────────────────

jclass global_class(JNIEnv* env, const char* name, bool system = false)
{
    jclass local = system ? env->FindClass(name) : vanta::jvm::findClassAnyLoader(env, name);
    if (clear_exc(env) || !local) return nullptr;
    jclass g = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return g;
}

jmethodID method(JNIEnv* env, jclass c, const char* n, const char* sig)
{
    if (!c) return nullptr;
    jmethodID m = env->GetMethodID(c, n, sig);
    clear_exc(env);
    return m;
}

jfieldID field(JNIEnv* env, jclass c, const char* n, const char* sig)
{
    if (!c) return nullptr;
    jfieldID f = env->GetFieldID(c, n, sig);
    clear_exc(env);
    return f;
}

jobject static_object_global(JNIEnv* env, jclass c, const char* n, const char* sig)
{
    if (!c) return nullptr;
    jfieldID f = env->GetStaticFieldID(c, n, sig);
    if (clear_exc(env) || !f) return nullptr;
    jobject local = env->GetStaticObjectField(c, f);
    if (clear_exc(env) || !local) return nullptr;
    jobject g = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return g;
}

void build_cache(JNIEnv* env)
{
    if (g_c.tried) return;
    g_c.tried = true;

    g_c.mc_class         = global_class(env, sdk::mappings::minecraftclass_sig);
    g_c.entity_hit_class = global_class(env, "net/minecraft/class_3966");
    g_c.block_hit_class  = global_class(env, "net/minecraft/class_3965");
    g_c.living_class     = global_class(env, "net/minecraft/class_1309");
    g_c.integer_class    = global_class(env, "java/lang/Integer", /*system=*/true);

    // Classes only needed to resolve ids / static refs; not kept.
    jclass entity_class   = global_class(env, "net/minecraft/class_1297");
    jclass stack_class    = global_class(env, "net/minecraft/class_1799");
    jclass item_class     = global_class(env, "net/minecraft/class_1792");
    jclass world_class    = global_class(env, "net/minecraft/class_1937");
    jclass state_class    = global_class(env, "net/minecraft/class_2680");
    jclass blocks_class   = global_class(env, "net/minecraft/class_2246");
    jclass anchor_class   = global_class(env, "net/minecraft/class_4969");
    jclass pos_class      = global_class(env, "net/minecraft/class_2338");

    g_c.player_fid       = field(env, g_c.mc_class, "field_1724", "Lnet/minecraft/class_746;");
    g_c.world_fid        = field(env, g_c.mc_class, "field_1687", "Lnet/minecraft/class_638;");
    g_c.crosshair_fid    = field(env, g_c.mc_class, "field_1765", "Lnet/minecraft/class_239;");
    g_c.use_cooldown_fid = field(env, g_c.mc_class, "field_1752", "I");

    g_c.get_entity_mid      = method(env, g_c.entity_hit_class, "method_17782", "()Lnet/minecraft/class_1297;");
    g_c.get_block_pos_mid   = method(env, g_c.block_hit_class,  "method_17777", "()Lnet/minecraft/class_2338;");
    g_c.is_blocking_mid     = method(env, g_c.living_class,     "method_6039",  "()Z");
    g_c.main_hand_mid       = method(env, g_c.living_class,     "method_6047",  "()Lnet/minecraft/class_1799;");
    g_c.is_sneaking_mid     = method(env, entity_class,         "method_5715",  "()Z");
    g_c.get_item_mid        = method(env, stack_class,          "method_7909",  "()Lnet/minecraft/class_1792;");
    g_c.translation_mid     = method(env, item_class,           "method_7876",  "()Ljava/lang/String;");
    g_c.get_block_state_mid = method(env, world_class,          "method_8320",  "(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2680;");
    g_c.get_block_mid       = method(env, state_class,          "method_26204", "()Lnet/minecraft/class_2248;");
    g_c.state_get_mid       = method(env, state_class,          "method_11654", "(Lnet/minecraft/class_2769;)Ljava/lang/Comparable;");
    g_c.int_value_mid       = method(env, g_c.integer_class,    "intValue",     "()I");
    g_c.pos_x_mid           = method(env, pos_class,            "method_10263", "()I");
    g_c.pos_y_mid           = method(env, pos_class,            "method_10264", "()I");
    g_c.pos_z_mid           = method(env, pos_class,            "method_10260", "()I");

    g_c.respawn_anchor_ref = static_object_global(env, blocks_class, "field_23152", "Lnet/minecraft/class_2248;");
    g_c.ender_chest_ref    = static_object_global(env, blocks_class, "field_10443", "Lnet/minecraft/class_2248;");
    g_c.charges_ref        = static_object_global(env, anchor_class, "field_23153", "Lnet/minecraft/class_2758;");

    auto drop = [&](jclass c) { if (c) env->DeleteGlobalRef(c); };
    drop(entity_class); drop(stack_class); drop(item_class); drop(world_class);
    drop(state_class); drop(blocks_class); drop(anchor_class); drop(pos_class);

    g_c.ready = g_c.mc_class && g_c.entity_hit_class && g_c.block_hit_class && g_c.living_class
             && g_c.integer_class
             && g_c.player_fid && g_c.world_fid && g_c.crosshair_fid && g_c.use_cooldown_fid
             && g_c.get_entity_mid && g_c.get_block_pos_mid && g_c.is_blocking_mid
             && g_c.main_hand_mid && g_c.is_sneaking_mid && g_c.get_item_mid
             && g_c.translation_mid && g_c.get_block_state_mid && g_c.get_block_mid
             && g_c.state_get_mid && g_c.int_value_mid
             && g_c.pos_x_mid && g_c.pos_y_mid && g_c.pos_z_mid
             && g_c.respawn_anchor_ref && g_c.ender_chest_ref && g_c.charges_ref;

    VLOG_INFO("antiaction",
              "cache: mc={} ehit={} bhit={} living={} integer={} player={} world={} crosshair={} "
              "useCooldown={} getEntity={} getBlockPos={} isBlocking={} mainHand={} sneaking={} "
              "getItem={} translationKey={} getBlockState={} getBlock={} stateGet={} intValue={} "
              "posXYZ={} RESPAWN_ANCHOR={} ENDER_CHEST={} CHARGES={} -> {}",
              g_c.mc_class ? 1 : 0, g_c.entity_hit_class ? 1 : 0, g_c.block_hit_class ? 1 : 0,
              g_c.living_class ? 1 : 0, g_c.integer_class ? 1 : 0,
              g_c.player_fid ? 1 : 0, g_c.world_fid ? 1 : 0, g_c.crosshair_fid ? 1 : 0,
              g_c.use_cooldown_fid ? 1 : 0, g_c.get_entity_mid ? 1 : 0, g_c.get_block_pos_mid ? 1 : 0,
              g_c.is_blocking_mid ? 1 : 0, g_c.main_hand_mid ? 1 : 0, g_c.is_sneaking_mid ? 1 : 0,
              g_c.get_item_mid ? 1 : 0, g_c.translation_mid ? 1 : 0, g_c.get_block_state_mid ? 1 : 0,
              g_c.get_block_mid ? 1 : 0, g_c.state_get_mid ? 1 : 0, g_c.int_value_mid ? 1 : 0,
              (g_c.pos_x_mid && g_c.pos_y_mid && g_c.pos_z_mid) ? 1 : 0,
              g_c.respawn_anchor_ref ? 1 : 0, g_c.ender_chest_ref ? 1 : 0, g_c.charges_ref ? 1 : 0,
              g_c.ready ? "ready" : "NOT READY - module inert");
}

// Resolves the MinecraftClient singleton (local ref) for the fallback pump.
jobject minecraft_instance(JNIEnv* env)
{
    if (!g_c.mc_class) return nullptr;
    jfieldID inst = env->GetStaticFieldID(g_c.mc_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (clear_exc(env) || !inst) return nullptr;
    jobject mc = env->GetStaticObjectField(g_c.mc_class, inst);
    if (clear_exc(env)) return nullptr;
    return mc;
}

// ─── the use rules ───────────────────────────────────────────────────────────

// Returns true and fills `reason` when this use has to be eaten.
bool evaluate_use(JNIEnv* env, jobject mc, char* reason, size_t reason_n)
{
    const bool r_double = g_rule_double_glowstone.load(std::memory_order_relaxed);
    const bool r_echest = g_rule_open_echest.load(std::memory_order_relaxed);
    const bool r_place  = g_rule_place_glowstone.load(std::memory_order_relaxed);
    if (!r_double && !r_echest && !r_place) return false;

    jobject player = env->GetObjectField(mc, g_c.player_fid);
    if (clear_exc(env) || !player) return false;

    const std::string key = main_hand_key(env, player);
    const bool glow   = is_glowstone(key);
    const bool combat = is_combat_item(key);
    if (!glow && !combat) { env->DeleteLocalRef(player); return false; }
    // (glowstone is itself a combat item, so `combat` covers both from here)

    bool blocked = false;
    jobject hit = env->GetObjectField(mc, g_c.crosshair_fid);
    clear_exc(env);

    // No hit result at all: vanilla logs "Null returned as 'hitResult'" and
    // then uses the item in hand anyway, which for glowstone is a placement
    // nowhere useful.
    if (!hit) {
        if (r_place && glow) {
            std::snprintf(reason, reason_n, "place glowstone (no target)");
            blocked = true;
        }
        env->DeleteLocalRef(player);
        return blocked;
    }

    if (env->IsInstanceOf(hit, g_c.block_hit_class) == JNI_TRUE) {
        jobject pos = nullptr, state = nullptr;
        jobject block = block_under(env, mc, hit, pos, state);
        if (block) {
            const bool anchor = env->IsSameObject(block, g_c.respawn_anchor_ref) == JNI_TRUE;
            const bool echest = env->IsSameObject(block, g_c.ender_chest_ref) == JNI_TRUE;
            char at[48];
            format_pos(env, pos, at, sizeof(at));

            if (anchor && glow) {
                const bool sneaking = player_sneaking(env, player);
                if (!sneaking) {
                    // (a) charged anchor + glowstone in hand = another charge
                    // instead of the explosion the fight wanted.
                    if (r_double) {
                        const int charges = anchor_charges(env, state);
                        if (charges >= 1) {
                            std::snprintf(reason, reason_n, "double glowstone on %s (charges %d)", at, charges);
                            blocked = true;
                        }
                    }
                } else if (r_place) {
                    // Sneaking skips the anchor's own use and turns the click
                    // into a placement AGAINST it -- glowstone outside an anchor.
                    std::snprintf(reason, reason_n, "place glowstone beside anchor at %s (sneaking)", at);
                    blocked = true;
                }
            } else if (echest) {
                // (c) an ender chest under the crosshair with combat items in
                // hand: the click would open it (or, sneaking, place against it).
                if (r_echest && combat) {
                    std::snprintf(reason, reason_n, "open e-chest at %s", at);
                    blocked = true;
                }
            } else if (glow && r_place) {
                // (b) any other block (air on a miss included): a placement.
                std::snprintf(reason, reason_n, "place glowstone on %s", at);
                blocked = true;
            }
            env->DeleteLocalRef(block);
        }
        if (state) env->DeleteLocalRef(state);
        if (pos)   env->DeleteLocalRef(pos);
    }
    // An entity hit passes: with glowstone in hand the use is an entity
    // interaction first, and there is no anchor or chest in that path.

    env->DeleteLocalRef(hit);
    env->DeleteLocalRef(player);
    return blocked;
}

// ─── the detour ──────────────────────────────────────────────────────────────

jint read_use_cooldown(JNIEnv* env, jobject mc)
{
    const jint v = env->GetIntField(mc, g_c.use_cooldown_fid);
    return clear_exc(env) ? -1 : v;
}

// SEH-only: no C++ objects with destructors in here.
void call_original_seh(JNIEnv* env, jobject thiz)
{
    __try {
        env->CallNonvirtualVoidMethod(thiz, g_c.mc_class, ORIG_doItemUse);
        if (env->ExceptionCheck()) env->ExceptionClear();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void JNICALL hkDoItemUse(JNIEnv* env, jobject thiz)
{
    vanta::NativeCallback callbackLifetime;
    if (!ORIG_doItemUse || !g_c.mc_class) return;

    const bool pass_through = g_shutting_down.load(std::memory_order_acquire)
                           || g_probe_failed.load(std::memory_order_acquire);

    if (!pass_through && g_probe_passed.load(std::memory_order_acquire)) {
        // The observer goes first: a click it takes over is no longer a click
        // this module has an opinion about. Only once the probe has proven the
        // original runs -- an observer that swallowed clicks during the probe
        // would keep the cooldown from moving and fail it.
        if (const antiaction::UseObserver fn = g_use_observer.load(std::memory_order_acquire)) {
            const bool consumed = fn(env, thiz, g_use_observer_user.load(std::memory_order_acquire));
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (consumed) return;   // no original, no packet, no swing
        }
        if (antiaction::should_block_use(env, thiz)) {
            return;   // no original, no packet, no swing
        }
    }

    if (g_probe_passed.load(std::memory_order_acquire) || pass_through) {
        call_original_seh(env, thiz);
        return;
    }

    // Still probing: the gate above is bypassed until the original is proven.
    const jint before = read_use_cooldown(env, thiz);
    call_original_seh(env, thiz);
    const jint after = read_use_cooldown(env, thiz);
    const int calls = g_probe_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (before >= 0 && after >= 0 && after != before) {
        g_probe_passed.store(true, std::memory_order_release);
        VLOG_INFO("antiaction", "doItemUse probe: original RUNS (itemUseCooldown {} -> {}) -- hook live",
                  static_cast<int>(before), static_cast<int>(after));
    } else if (calls >= kProbeCalls) {
        g_probe_failed.store(true, std::memory_order_release);
        VLOG_WARN("antiaction", "doItemUse probe: original DID NOT RUN in {} calls "
                  "(itemUseCooldown stayed {}) -- detaching, window fallback takes over",
                  calls, static_cast<int>(after));
    }
}

// SEH-only wrappers around the JNIHook calls. `result` gets the attach code,
// `orig` the trampoline to the copy-class original.
int attach_seh(jmethodID mid, jmethodID* orig)
{
    int r = static_cast<int>(JNIHOOK_ERR_JVMTI_OPERATION);
    __try {
        r = static_cast<int>(JNIHook_Attach(mid, reinterpret_cast<void*>(hkDoItemUse), orig));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    return r;
}

void detach_seh(jmethodID mid)
{
    __try { JNIHook_Detach(mid); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Runs on the game thread from the pump, once. Any failure lands in Failed and
// the fallback takes over for the rest of the process.
void install_hook(JNIEnv* env)
{
    if (g_hook_state.load(std::memory_order_acquire) != static_cast<int>(HookState::Idle)) return;
    if (!g_c.ready) {
        g_hook_state.store(static_cast<int>(HookState::Failed), std::memory_order_release);
        VLOG_WARN("antiaction", "install: cache not ready -- window fallback only");
        return;
    }

    jmethodID mid = env->GetMethodID(g_c.mc_class, "method_1583", "()V");
    if (clear_exc(env) || !mid) {
        g_hook_state.store(static_cast<int>(HookState::Failed), std::memory_order_release);
        VLOG_WARN("antiaction", "install: class_310.method_1583 ()V not found -- window fallback only");
        return;
    }

    const jnihook_result_t init = JNIHook_Init(vanta::jvm::ctx().vm.load());
    if (init != JNIHOOK_OK && init != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        g_hook_state.store(static_cast<int>(HookState::Failed), std::memory_order_release);
        VLOG_WARN("antiaction", "install: JNIHook_Init failed ({}) -- window fallback only",
                  static_cast<int>(init));
        return;
    }

    jmethodID orig = nullptr;
    const int r = attach_seh(mid, &orig);
    if (r != static_cast<int>(JNIHOOK_OK) || !orig) {
        g_hook_state.store(static_cast<int>(HookState::Failed), std::memory_order_release);
        VLOG_WARN("antiaction", "install: JNIHook_Attach(doItemUse) failed (r={}) -- window fallback only", r);
        return;
    }

    ORIG_doItemUse    = orig;
    g_do_item_use_mid = mid;
    g_probe_calls.store(0, std::memory_order_relaxed);
    g_probe_passed.store(false, std::memory_order_relaxed);
    g_probe_failed.store(false, std::memory_order_relaxed);
    g_hook_state.store(static_cast<int>(HookState::Live), std::memory_order_release);
    VLOG_INFO("antiaction", "install: doItemUse attached, verifying on the next right-click");
}

void detach_hook()
{
    if (g_hook_state.load(std::memory_order_acquire) != static_cast<int>(HookState::Live)) return;
    if (g_do_item_use_mid) detach_seh(g_do_item_use_mid);
    ORIG_doItemUse    = nullptr;
    g_do_item_use_mid = nullptr;
    g_probe_passed.store(false, std::memory_order_release);
}

bool hook_live()
{
    return g_hook_state.load(std::memory_order_acquire) == static_cast<int>(HookState::Live)
        && g_probe_passed.load(std::memory_order_acquire)
        && !g_probe_failed.load(std::memory_order_acquire);
}

} // namespace

// ─── gates ───────────────────────────────────────────────────────────────────

namespace antiaction {

bool should_block_attack(JNIEnv* env, jobject mc)
{
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (!g_rule_sword_shield.load(std::memory_order_relaxed)) return false;
    if (g_shutting_down.load(std::memory_order_acquire)) return false;
    if (!g_c.ready || !env || !mc) return false;

    jobject hit = env->GetObjectField(mc, g_c.crosshair_fid);
    if (clear_exc(env) || !hit) return false;

    bool block = false;
    if (env->IsInstanceOf(hit, g_c.entity_hit_class) == JNI_TRUE) {
        jobject target = env->CallObjectMethod(hit, g_c.get_entity_mid);
        if (!clear_exc(env) && target) {
            if (env->IsInstanceOf(target, g_c.living_class) == JNI_TRUE) {
                const jboolean blocking = env->CallBooleanMethod(target, g_c.is_blocking_mid);
                if (!clear_exc(env) && blocking == JNI_TRUE) {
                    jobject player = env->GetObjectField(mc, g_c.player_fid);
                    if (!clear_exc(env) && player) {
                        block = is_sword(main_hand_key(env, player));
                        env->DeleteLocalRef(player);
                    }
                }
            }
            env->DeleteLocalRef(target);
        }
    }
    env->DeleteLocalRef(hit);

    if (block) note_blocked("sword into a raised shield");
    return block;
}

bool should_block_use(JNIEnv* env, jobject mc)
{
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (g_shutting_down.load(std::memory_order_acquire)) return false;
    if (!g_c.ready || !env || !mc) return false;

    char reason[128] = {};
    if (!evaluate_use(env, mc, reason, sizeof(reason))) return false;
    note_blocked(reason);
    return true;
}

void set_use_observer(UseObserver fn, void* user)
{
    if (!fn) {
        // Clear the function first: a detour racing this read sees "no
        // observer" before it could see a user pointer that is going away.
        g_use_observer.store(nullptr, std::memory_order_release);
        g_use_observer_user.store(nullptr, std::memory_order_release);
        return;
    }
    g_use_observer_user.store(user, std::memory_order_release);
    g_use_observer.store(fn, std::memory_order_release);
}

bool use_hook_live()
{
    return !g_shutting_down.load(std::memory_order_acquire) && hook_live();
}

} // namespace antiaction

// ─── module ──────────────────────────────────────────────────────────────────

class AntiAction : public Module {
public:
    AntiAction()
        : Module("Anti Action", "Prevents certain bad actions from interrupting your pvp", Category::Combat)
        , doubleGlowstone_(this, "Double Glowstone", true, "Prevents double filling an anchor with glowstone")
        , openEChest_(this, "Open E-Chest", true, "Prevents opening an ender chest while holding combat items")
        , placeGlowstone_(this, "Place Glowstone", true, "Prevents placing glowstone outside of an anchor")
        , swordShield_(this, "Sword Shield", true, "Prevents you from hitting a shield with a sword")
    {
        registerSetting(&doubleGlowstone_);
        registerSetting(&openEChest_);
        registerSetting(&placeGlowstone_);
        registerSetting(&swordShield_);
    }

    ~AntiAction() override
    {
        // ModuleManager::shutdownAll destroys modules while the JVM is still
        // up (jvm::shutdown comes after it in Bootstrap::shutdown), so this is
        // where the detour leaves the class -- it must not outlive the DLL.
        g_shutting_down.store(true, std::memory_order_release);
        g_enabled.store(false, std::memory_order_release);
        ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_ANTIACTION, 0);
        if (vanta::jvm::thisThreadEnv()) detach_hook();
    }

    void onInit() override
    {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){ pump(); });
    }

    std::string displayInfo() const override
    {
        std::lock_guard<std::mutex> lk(g_reason_mutex);
        if (g_reason[0] == '\0') return {};
        if (GetTickCount64() - g_reason_ms > kReasonShowMs) return {};
        return std::string(g_reason);
    }

protected:
    void onEnable() override
    {
        // The hook itself is installed from the pump: onEnable can arrive from
        // the keybind thread, and a class redefinition belongs on the game
        // thread like every other JNIHook in this client.
        g_enabled.store(true, std::memory_order_release);
    }

    void onDisable() override
    {
        g_enabled.store(false, std::memory_order_release);
        filter_raised_ = false;
        ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_ANTIACTION, 0);
        // The detour stays attached and passes through while disabled; a
        // re-enable must not redefine class_310 again.
    }

private:
    void pump()
    {
        if (g_shutting_down.load(std::memory_order_acquire)) return;
        // The hook is wanted by this module's rules OR by a registered use
        // observer (Safe Anchor); the rules themselves only run while enabled.
        const bool enabled  = g_enabled.load(std::memory_order_acquire);
        const bool observed = g_use_observer.load(std::memory_order_acquire) != nullptr;
        if (!enabled && !observed) return;

        if (enabled) {
            g_rule_double_glowstone.store(doubleGlowstone_.value(), std::memory_order_relaxed);
            g_rule_open_echest.store(openEChest_.value(), std::memory_order_relaxed);
            g_rule_place_glowstone.store(placeGlowstone_.value(), std::memory_order_relaxed);
            g_rule_sword_shield.store(swordShield_.value(), std::memory_order_relaxed);
        }

        vanta::jvm::JniScope scope;
        JNIEnv* env = scope.get();
        if (!env) return;

        build_cache(env);
        if (!g_c.ready) return;

        install_hook(env);

        // A probe that failed inside the detour asked for a detach; do it here,
        // outside the detour, on the game thread.
        if (g_probe_failed.load(std::memory_order_acquire)
            && g_hook_state.load(std::memory_order_acquire) == static_cast<int>(HookState::Live)) {
            detach_hook();
            g_hook_state.store(static_cast<int>(HookState::Failed), std::memory_order_release);
        }

        if (!enabled) {
            // Observer only: the hook is up (or failed) and that is all that was
            // wanted. onDisable already dropped the window filter.
            return;
        }

        if (hook_live()) {
            if (filter_raised_) {
                filter_raised_ = false;
                ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_ANTIACTION, 0);
            }
            return;
        }

        // Window-level fallback (hook unavailable, or still being verified):
        // swallow mouse buttons at the window while a use that would have been
        // blocked is lined up. Left clicks are eaten too at level 1, which is
        // the price of this path -- the hook path has no such cost.
        jobject mc = minecraft_instance(env);
        bool want = false;
        if (mc) {
            char reason[128] = {};
            want = evaluate_use(env, mc, reason, sizeof(reason));
            if (want && !filter_raised_) note_blocked(reason);
            env->DeleteLocalRef(mc);
        }
        if (want != filter_raised_) {
            filter_raised_ = want;
            ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_ANTIACTION,
                                                     want ? 1 : 0);
        }
    }

    BoolSetting doubleGlowstone_;
    BoolSetting openEChest_;
    BoolSetting placeGlowstone_;
    BoolSetting swordShield_;
    bool filter_raised_ = false;
};

VANTA_REGISTER_MODULE(AntiAction);

} // namespace vanta::modules
