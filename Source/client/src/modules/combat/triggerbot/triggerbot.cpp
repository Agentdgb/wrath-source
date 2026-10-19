// triggerbot.cpp — Native (C++ / JNIHook) triggerbot for Vanta.
//
// Ported from the enhance-client native triggerbot. Drives itself from a
// JNIHook installed in triggerbot_hook (MinecraftClient.doAttack) plus a
// world-tick sample point piggy-backed on sendMovementPackets (see the
// note on hkTickMovement being removed under JVM 21).
//
// Attack dispatch is a real synthetic left-click via SendInput so the
// game's own un-hooked handleInputEvents fires doAttack() at its natural
// tick phase → genuine vanilla swing animation, no desync, no missed hits.
//
// This file owns the JNI reflection cache + lifecycle. The per-tick
// logic and the JNIHook itself live in triggerbot_hook.cpp.

#include "triggerbot.h"
#include "triggerbot_hook.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include <sdk/mappings/mappings.hpp>
#include <windows.h>
#include <mutex>

namespace vanta {
namespace modules {
namespace triggerbot {

static jni_cache  g_cache;
static std::mutex s_cache_mx;

const jni_cache& get_cache() { return g_cache; }

static bool cache_class(JNIEnv* env, const char* sig, jclass& out_class)
{
    if (!sig) return false;
    jclass local = vanta::jvm::findClassAnyLoader(env, sig);
    if (!local)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    out_class = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return out_class != nullptr;
}

static jmethodID cache_method(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jmethodID mid = env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return mid;
}

static jfieldID cache_field(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID fid = env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return fid;
}

void init_cache(JNIEnv* env)
{
    if (!env || g_cache.initialized) return;
    std::scoped_lock lk(s_cache_mx);
    if (g_cache.initialized) return;

    using namespace sdk::mappings;

    cache_class(env, minecraftclass_sig, g_cache.minecraft_class);
    if (g_cache.minecraft_class)
    {
        g_cache.minecraft_crosshair_target_fid =
            cache_field(env, g_cache.minecraft_class, crosshair_target_name, crosshair_target_sig);
        g_cache.minecraft_current_screen_fid =
            cache_field(env, g_cache.minecraft_class,
                minecraft_current_screen_name, minecraft_current_screen_sig);
        g_cache.minecraft_player_fid =
            cache_field(env, g_cache.minecraft_class, player_name, player_sig);
        g_cache.minecraft_interaction_manager_fid =
            cache_field(env, g_cache.minecraft_class,
                interaction_manager_name, interaction_manager_sig);
        g_cache.minecraft_attack_cooldown_fid =
            cache_field(env, g_cache.minecraft_class,
                attack_cooldown_name, attack_cooldown_sig);
        g_cache.minecraft_world_fid =
            cache_field(env, g_cache.minecraft_class, world_name, world_sig);
    }

    // ClientWorld → entities iterable. Needed for the extended-reach
    // raycast that scans past vanilla's ~3 block crosshair pick.
    cache_class(env, client_world_class_sig, g_cache.world_class);
    if (g_cache.world_class)
    {
        g_cache.world_get_entities_mid = cache_method(env, g_cache.world_class,
            world_get_entities_name, world_get_entities_sig);
    }

    cache_class(env, "java/lang/Iterable", g_cache.iterable_class);
    if (g_cache.iterable_class)
    {
        g_cache.iterable_iterator_mid = cache_method(env, g_cache.iterable_class,
            "iterator", "()Ljava/util/Iterator;");
    }
    cache_class(env, "java/util/Iterator", g_cache.iterator_class);
    if (g_cache.iterator_class)
    {
        g_cache.iterator_has_next_mid = cache_method(env, g_cache.iterator_class,
            "hasNext", "()Z");
        g_cache.iterator_next_mid = cache_method(env, g_cache.iterator_class,
            "next", "()Ljava/lang/Object;");
    }

    cache_class(env, interaction_manager_class_sig, g_cache.interaction_manager_class);
    if (g_cache.interaction_manager_class)
    {
        g_cache.interaction_manager_attack_entity_mid = cache_method(env,
            g_cache.interaction_manager_class,
            attack_entity_name, attack_entity_sig);
    }

    cache_class(env, entity_hit_result_class_sig, g_cache.entity_hit_result_class);
    if (g_cache.entity_hit_result_class)
    {
        g_cache.entity_hit_result_get_entity_mid = cache_method(env,
            g_cache.entity_hit_result_class,
            entity_hit_result_get_entity_name,
            entity_hit_result_get_entity_sig);
        // v124: <init>(Entity) — silent aim spoof usa este ctor para envolver
        // el target y setearlo como MC.crosshairTarget durante hkDoAttack.
        g_cache.entity_hit_result_ctor = env->GetMethodID(
            g_cache.entity_hit_result_class, "<init>", "(Lnet/minecraft/class_1297;)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_cache.entity_hit_result_ctor = nullptr; }
    }

    cache_class(env, player_entity_class_sig, g_cache.player_entity_class);
    if (g_cache.player_entity_class)
    {
        g_cache.player_get_attack_cooldown_progress_mid = cache_method(env,
            g_cache.player_entity_class,
            get_attack_cooldown_progress_name,
            get_attack_cooldown_progress_sig);
        g_cache.player_reset_last_attacked_mid = cache_method(env,
            g_cache.player_entity_class,
            reset_last_attacked_ticks_name,
            reset_last_attacked_ticks_sig);
        g_cache.player_inventory_fid =
            cache_field(env, g_cache.player_entity_class, player_inventory_name, player_inventory_sig);
    }

    cache_class(env, living_entity_class_sig, g_cache.living_entity_class);
    if (g_cache.living_entity_class)
    {
        g_cache.living_entity_get_health_mid = cache_method(env,
            g_cache.living_entity_class,
            living_entity_get_health_name,
            living_entity_get_health_sig);
        g_cache.living_entity_hurt_time_fid = cache_field(env,
            g_cache.living_entity_class,
            living_entity_hurt_time_name,
            living_entity_hurt_time_sig);
        g_cache.living_entity_is_using_item_mid = cache_method(env,
            g_cache.living_entity_class,
            living_entity_is_using_item_name,
            living_entity_is_using_item_sig);
        g_cache.living_entity_is_blocking_mid = cache_method(env,
            g_cache.living_entity_class,
            living_entity_is_blocking_name,
            living_entity_is_blocking_sig);
        g_cache.living_entity_swing_hand_mid = cache_method(env,
            g_cache.living_entity_class,
            swing_hand_name, swing_hand_sig);
    }

    cache_class(env, hand_class_sig, g_cache.hand_class);
    if (g_cache.hand_class)
    {
        jfieldID main_fid = env->GetStaticFieldID(g_cache.hand_class,
            hand_main_hand_name, hand_main_hand_sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (main_fid)
        {
            jobject local = env->GetStaticObjectField(g_cache.hand_class, main_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (local)
            {
                g_cache.hand_main_hand_ref = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
    }

    if (g_cache.player_entity_class)
        g_cache.player_class = (jclass)env->NewGlobalRef(g_cache.player_entity_class);

    cache_class(env, entity_class_sig, g_cache.entity_class);
    if (g_cache.entity_class)
    {
        g_cache.entity_get_x_mid = cache_method(env, g_cache.entity_class,
            entity_get_x_name, entity_get_x_sig);
        g_cache.entity_get_y_mid = cache_method(env, g_cache.entity_class,
            entity_get_y_name, entity_get_y_sig);
        g_cache.entity_get_z_mid = cache_method(env, g_cache.entity_class,
            entity_get_z_name, entity_get_z_sig);
        g_cache.entity_get_eye_y_mid = cache_method(env, g_cache.entity_class,
            "method_23320", "()D");
        if (!g_cache.entity_get_eye_y_mid)
            g_cache.entity_get_eye_y_mid = cache_method(env, g_cache.entity_class,
                "getEyeY", "()D");
        g_cache.entity_get_box_mid = cache_method(env, g_cache.entity_class,
            "method_5829", "()Lnet/minecraft/class_238;");
        if (!g_cache.entity_get_box_mid)
            g_cache.entity_get_box_mid = cache_method(env, g_cache.entity_class,
                "getBoundingBox", "()Lnet/minecraft/world/phys/AABB;");
        g_cache.entity_is_on_ground_mid = cache_method(env, g_cache.entity_class,
            is_on_ground_name, is_on_ground_sig);
        g_cache.entity_is_invisible_mid = env->GetMethodID(g_cache.entity_class,
            "method_5767", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!g_cache.entity_is_invisible_mid) {
            g_cache.entity_is_invisible_mid = env->GetMethodID(g_cache.entity_class,
                "isInvisible", "()Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        g_cache.entity_is_sprinting_mid = cache_method(env, g_cache.entity_class,
            is_sprinting_name, is_sprinting_sig);
        g_cache.entity_is_touching_water_mid = cache_method(env, g_cache.entity_class,
            entity_is_touching_water_name, entity_is_touching_water_sig);
        g_cache.entity_velocity_fid = cache_field(env, g_cache.entity_class,
            entity_velocity_name, entity_velocity_sig);
        // Entity.prevY — needed for Vape's crit test: getY() - prevY < 0.
        // Same field the aimassist port uses; intermediary field_6036 / mojmap "yo".
        g_cache.entity_prev_y_fid = cache_field(env, g_cache.entity_class,
            sdk::mappings::is_mojang_layout ? "yo" : "field_6036", "D");
        if (!g_cache.entity_prev_y_fid)
            g_cache.entity_prev_y_fid = cache_field(env, g_cache.entity_class,
                sdk::mappings::is_mojang_layout ? "field_6036" : "yo", "D");
        // Rotation fields for the extended-reach raycast — Entity.yaw /
        // Entity.pitch (NOT the ClientPlayerEntity render-interpolated
        // yaw/pitch). Yarn intermediary names are the authoritative fallback;
        // mojmap ships them as "yRot" / "xRot" so try those first when
        // Mojang mappings are in force.
        const bool mojang = sdk::mappings::is_mojang_layout;
        g_cache.entity_yaw_fid = cache_field(env, g_cache.entity_class,
            mojang ? "yRot" : "field_6031", "F");
        if (!g_cache.entity_yaw_fid)
            g_cache.entity_yaw_fid = cache_field(env, g_cache.entity_class,
                mojang ? "field_6031" : "yRot", "F");

        g_cache.entity_pitch_fid = cache_field(env, g_cache.entity_class,
            mojang ? "xRot" : "field_5965", "F");
        if (!g_cache.entity_pitch_fid)
            g_cache.entity_pitch_fid = cache_field(env, g_cache.entity_class,
                mojang ? "field_5965" : "xRot", "F");

        g_cache.entity_prev_yaw_fid = cache_field(env, g_cache.entity_class,
            mojang ? "yRotO" : "field_5982", "F");
        if (!g_cache.entity_prev_yaw_fid)
            g_cache.entity_prev_yaw_fid = cache_field(env, g_cache.entity_class,
                mojang ? "field_5982" : "yRotO", "F");

        g_cache.entity_prev_pitch_fid = cache_field(env, g_cache.entity_class,
            mojang ? "xRotO" : "field_6004", "F");
        if (!g_cache.entity_prev_pitch_fid)
            g_cache.entity_prev_pitch_fid = cache_field(env, g_cache.entity_class,
                mojang ? "field_6004" : "xRotO", "F");

        // fallDistance: F in 1.21.1-1.21.4, D in 1.21.5+. The mappings probe
        // sets the signature, but if it ran before the entity class was
        // visible (classloader race) it can leave the signature stale and
        // the cache silently goes null → can_crit always returns false →
        // Prioritize crits never fires. Belt-and-braces: try the configured
        // sig first, then fall back to the OTHER type and update the global
        // flag so can_crit reads the right type.
        g_cache.entity_fall_distance_fid = cache_field(env, g_cache.entity_class,
            entity_fall_distance_name, entity_fall_distance_sig);
        if (!g_cache.entity_fall_distance_fid)
        {
            const char* alt_sig = (sdk::mappings::fall_distance_is_float) ? "D" : "F";
            jfieldID alt = cache_field(env, g_cache.entity_class,
                entity_fall_distance_name, alt_sig);
            if (alt)
            {
                g_cache.entity_fall_distance_fid = alt;
                sdk::mappings::fall_distance_is_float = (alt_sig[0] == 'F');
                entity_fall_distance_sig = alt_sig;
            }
        }
    }

    if (!cache_class(env, "net/minecraft/class_238", g_cache.box_class))
        cache_class(env, "net/minecraft/world/phys/AABB", g_cache.box_class);
    if (g_cache.box_class)
    {
        auto box_f = [&](const char* yarn, const char* moj) -> jfieldID {
            jfieldID f = cache_field(env, g_cache.box_class, yarn, "D");
            if (!f) f = cache_field(env, g_cache.box_class, moj, "D");
            return f;
        };
        g_cache.box_min_x_fid = box_f("field_1323", "minX");
        g_cache.box_min_y_fid = box_f("field_1322", "minY");
        g_cache.box_min_z_fid = box_f("field_1321", "minZ");
        g_cache.box_max_x_fid = box_f("field_1320", "maxX");
        g_cache.box_max_y_fid = box_f("field_1325", "maxY");
        g_cache.box_max_z_fid = box_f("field_1324", "maxZ");
    }

    cache_class(env, mob_entity_class_sig,         g_cache.mob_entity_class);
    cache_class(env, animal_entity_class_sig,      g_cache.animal_entity_class);
    cache_class(env, villager_entity_class_sig,    g_cache.villager_entity_class);
    cache_class(env, end_crystal_entity_class_sig, g_cache.end_crystal_entity_class);
    cache_class(env, armor_stand_entity_class_sig, g_cache.armor_stand_entity_class);

    cache_class(env, vec3d_class_sig, g_cache.vec3d_class);
    if (g_cache.vec3d_class)
    {
        g_cache.vec3d_y_fid = cache_field(env, g_cache.vec3d_class, vec3d_y_name, vec3d_y_sig);
        g_cache.vec3d_x_fid = cache_field(env, g_cache.vec3d_class, vec3d_x_name, vec3d_x_sig);
        g_cache.vec3d_z_fid = cache_field(env, g_cache.vec3d_class, vec3d_z_name, vec3d_z_sig);
    }

    cache_class(env, player_inventory_class_sig, g_cache.inventory_class);
    if (g_cache.inventory_class)
    {
        g_cache.inventory_selected_slot_fid = cache_field(env, g_cache.inventory_class,
            inventory_selected_slot_name, inventory_selected_slot_sig);
        g_cache.inventory_get_stack_mid = cache_method(env, g_cache.inventory_class,
            inventory_get_stack_name, inventory_get_stack_sig);
    }

    cache_class(env, "net/minecraft/class_1799", g_cache.itemstack_class);
    if (g_cache.itemstack_class)
    {
        g_cache.itemstack_is_empty_mid = cache_method(env, g_cache.itemstack_class,
            itemstack_is_empty_name, itemstack_is_empty_sig);
        g_cache.itemstack_get_item_mid = cache_method(env, g_cache.itemstack_class,
            itemstack_get_item_name, itemstack_get_item_sig);
    }

    cache_class(env, item_class_sig, g_cache.item_class);
    if (g_cache.item_class)
    {
        g_cache.item_get_translation_key_mid = cache_method(env, g_cache.item_class,
            item_get_translation_key_name, item_get_translation_key_sig);
    }

    bool valid = g_cache.minecraft_class && g_cache.minecraft_player_fid;
    g_cache.initialized = valid;
}

void shutdown_cache()
{
    // Detach the JNIHooks first so no callback touches the cache mid-teardown.
    triggerbot_hook::shutdown();

    std::scoped_lock lk(s_cache_mx);
    if (!g_cache.initialized) return;
    if (!vanta::jvm::ctx().vm.load()) { g_cache = jni_cache{}; return; }

    JNIEnv* env = vanta::jvm::thisThreadEnv();
    if (!env) { g_cache = jni_cache{}; return; }

    auto drop = [&](jclass& c) {
        if (c) { env->DeleteGlobalRef(c); c = nullptr; }
    };

    drop(g_cache.minecraft_class);
    drop(g_cache.interaction_manager_class);
    if (g_cache.hand_main_hand_ref) { env->DeleteGlobalRef(g_cache.hand_main_hand_ref); g_cache.hand_main_hand_ref = nullptr; }
    drop(g_cache.hand_class);
    drop(g_cache.entity_hit_result_class);
    drop(g_cache.player_entity_class);
    drop(g_cache.living_entity_class);
    drop(g_cache.player_class);
    drop(g_cache.entity_class);
    drop(g_cache.box_class);
    drop(g_cache.mob_entity_class);
    drop(g_cache.animal_entity_class);
    drop(g_cache.villager_entity_class);
    drop(g_cache.end_crystal_entity_class);
    drop(g_cache.armor_stand_entity_class);
    drop(g_cache.vec3d_class);
    drop(g_cache.inventory_class);
    drop(g_cache.itemstack_class);
    drop(g_cache.item_class);
    drop(g_cache.world_class);
    drop(g_cache.iterable_class);
    drop(g_cache.iterator_class);

    g_cache = jni_cache{};
}

// Forward decl of the desktop-.txt logger defined in triggerbot_hook.cpp.
// Every gate / dispatch site writes into the same file so a single grep-able
// stream ends up in C:\Users\vldwd\Desktop\vanta_trigger.txt.
extern "C" void vanta_trigger_desktop_log(const char* fmt, ...);

void run()
{
    // Heartbeat every ~1 s so the user can see run() is even being reached.
    // Writes to C:\Users\vldwd\Desktop\vanta_trigger.txt via the same file
    // handle triggerbot_hook uses.
    {
        static unsigned long long s_next = 0;
        unsigned long long now = GetTickCount64();
        if (now >= s_next) {
            s_next = now + 1000;
            vanta_trigger_desktop_log(
                "TB[run]: heartbeat vm=%p env_ok=%d cache_init=%d enabled=%d hook_active=%d",
                (void*)vanta::jvm::ctx().vm.load(),
                (int)(vanta::jvm::thisThreadEnv() != nullptr),
                (int)g_cache.initialized,
                (int)settings::enabled,
                (int)triggerbot_hook::is_active());
        }
    }

    if (!vanta::jvm::ctx().vm.load()) return;
    JNIEnv* env = vanta::jvm::thisThreadEnv();
    if (!env) return;

    if (!g_cache.initialized) init_cache(env);

    // Install the doAttack JNIHook once. The tickMovement hook is
    // deliberately NOT installed — under MC 1.21.7+ / JVM 21 that
    // retransform generated a broken class copy that froze player
    // movement, and the ported hook layer still declares the callback
    // but init() no longer attaches it.
    if (!triggerbot_hook::is_active())
        triggerbot_hook::init();

    if (!settings::enabled) {
        triggerbot_hook::notify_disabled();
        (void)triggerbot_hook::consume_pending_on_hit();
        return;
    }

    // The core "why nothing hit" fix. Vanta bombs GameLoopEvent from the
    // wglSwapBuffers detour — the same call-site enhance used to drive
    // tick_from_render every frame. Without this line, hkDoAttack still
    // attaches but nothing ever calls try_trigger_attack, so SendInput
    // is never issued and the bot never fires.
    triggerbot_hook::tick_from_render(env);

    (void)triggerbot_hook::consume_pending_on_hit();
}

bool is_visual_focus_target(JNIEnv* env, jobject ent)
{
    // Both flags must be on for the highlight to show. We query the
    // raw "is the focused jobject" from the hook layer; the gating on
    // user toggles lives here so the hook stays oblivious to GUI state.
    if (!settings::focus_mode)         return false;
    if (!settings::show_focus_visuals) return false;
    return triggerbot_hook::is_focus_target_locked(env, ent);
}

}
}
}
