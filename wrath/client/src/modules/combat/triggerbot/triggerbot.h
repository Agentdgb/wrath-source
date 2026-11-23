#pragma once

#include <sdk/mappings/mappings.hpp>   // sdk::mappings intermediary strings
#include <jni.h>
#include <atomic>

namespace vanta::modules::triggerbot {

// Per-VM reflection cache. Populated once by init_cache() the first time
// the module fires; every JNI lookup after that is a hash-table hit.
struct jni_cache {
    bool        initialized = false;

    jclass      minecraft_class                     = nullptr;
    jfieldID    minecraft_crosshair_target_fid      = nullptr;
    jfieldID    minecraft_current_screen_fid        = nullptr;
    jfieldID    minecraft_player_fid                = nullptr;
    jfieldID    minecraft_interaction_manager_fid   = nullptr;
    jfieldID    minecraft_attack_cooldown_fid       = nullptr;
    jfieldID    minecraft_world_fid                 = nullptr; // ClientWorld / ClientLevel

    // Extended-reach raycast plumbing. Populated on first use — vanilla
    // Minecraft.crosshairTarget is capped at the game's own interaction
    // reach (≈3 blocks), so anything past that has to come from our own
    // walk over world entities.
    jclass      world_class                         = nullptr;
    jmethodID   world_get_entities_mid              = nullptr; // ()Iterable<Entity>
    jclass      iterable_class                      = nullptr;
    jmethodID   iterable_iterator_mid               = nullptr; // ()Iterator
    jclass      iterator_class                      = nullptr;
    jmethodID   iterator_has_next_mid               = nullptr; // ()Z
    jmethodID   iterator_next_mid                   = nullptr; // ()Object

    jfieldID    entity_yaw_fid                      = nullptr;
    jfieldID    entity_pitch_fid                    = nullptr;
    jfieldID    vec3d_x_fid                         = nullptr;
    jfieldID    vec3d_z_fid                         = nullptr;

    jclass      interaction_manager_class           = nullptr;
    jmethodID   interaction_manager_attack_entity_mid = nullptr;

    jclass      entity_hit_result_class             = nullptr;
    jmethodID   entity_hit_result_get_entity_mid    = nullptr;
    jmethodID   entity_hit_result_ctor              = nullptr;

    jclass      player_entity_class                 = nullptr;
    jmethodID   player_get_attack_cooldown_progress_mid = nullptr;
    jmethodID   player_reset_last_attacked_mid      = nullptr;
    jfieldID    player_inventory_fid                = nullptr;

    jclass      living_entity_class                 = nullptr;
    jmethodID   living_entity_get_health_mid        = nullptr;
    jfieldID    living_entity_hurt_time_fid         = nullptr;
    jmethodID   living_entity_is_using_item_mid     = nullptr;
    jmethodID   living_entity_is_blocking_mid       = nullptr;
    jmethodID   living_entity_swing_hand_mid        = nullptr;

    jclass      hand_class                          = nullptr;
    jobject     hand_main_hand_ref                  = nullptr;

    jclass      player_class                        = nullptr;

    jclass      entity_class                        = nullptr;
    jmethodID   entity_block_raycast_mid            = nullptr;
    jmethodID   hit_result_get_pos_mid              = nullptr;
    jmethodID   entity_get_eye_y_mid                = nullptr;
    jmethodID   entity_get_x_mid                    = nullptr;
    jmethodID   entity_get_y_mid                    = nullptr;
    jfieldID    entity_prev_y_fid                   = nullptr;   // Entity.prevY, for the crit test
    jmethodID   entity_get_z_mid                    = nullptr;
    jmethodID   entity_is_on_ground_mid             = nullptr;
    jmethodID   entity_is_invisible_mid             = nullptr;
    jmethodID   entity_is_alive_mid                 = nullptr;
    jmethodID   entity_is_spectator_mid             = nullptr;
    jmethodID   entity_is_removed_mid               = nullptr;
    jmethodID   entity_is_sprinting_mid             = nullptr;
    jmethodID   entity_set_sprinting_mid            = nullptr;
    jmethodID   entity_is_touching_water_mid        = nullptr;
    jfieldID    entity_velocity_fid                 = nullptr;
    jfieldID    entity_fall_distance_fid            = nullptr;

    jclass      mob_entity_class                    = nullptr;
    jclass      animal_entity_class                 = nullptr;
    jclass      villager_entity_class               = nullptr;
    jclass      end_crystal_entity_class            = nullptr;
    jclass      armor_stand_entity_class            = nullptr;

    jclass      vec3d_class                         = nullptr;
    jfieldID    vec3d_y_fid                         = nullptr;

    jclass      inventory_class                     = nullptr;
    jfieldID    inventory_selected_slot_fid         = nullptr;
    jmethodID   inventory_get_stack_mid             = nullptr;

    jclass      itemstack_class                     = nullptr;
    jmethodID   itemstack_is_empty_mid              = nullptr;
    jmethodID   itemstack_get_item_mid              = nullptr;

    jclass      item_class                          = nullptr;
    jmethodID   item_get_translation_key_mid        = nullptr;
};

// Runtime settings mirror. The ClickGui-facing Module writes into these;
// the hook layer reads them. Kept as a plain struct instead of the old
// scattered globals so all state travels together.
template <typename T>
struct AtomicSetting {
    std::atomic<T> v;
    constexpr AtomicSetting(T init) noexcept : v(init) {}
    operator T() const noexcept { return v.load(std::memory_order_acquire); }
    AtomicSetting& operator=(T next) noexcept { v.store(next, std::memory_order_release); return *this; }
};

struct settings {
    inline static AtomicSetting<bool> enabled { false };
    inline static AtomicSetting<bool> show_gui { false }; // "a GUI screen is open"
    inline static AtomicSetting<bool> work_in_screen { false };
    inline static AtomicSetting<bool> require_click { false };
    inline static AtomicSetting<bool> only_weapons { false };
    inline static AtomicSetting<bool> check_shield { true };
    inline static AtomicSetting<bool> check_teams { false };
    inline static AtomicSetting<int> max_hurt_time { 0 };
    inline static AtomicSetting<bool> wait_for_crit { true };   // prioritize crits
    inline static AtomicSetting<bool> full_charge_normal { false };
    inline static AtomicSetting<bool> focus_mode { true };
    inline static AtomicSetting<bool> only_on_hit { false };
    inline static AtomicSetting<float> post_eat_delay_ms { 100.0f };
    inline static AtomicSetting<bool> show_focus_visuals { false };
    inline static AtomicSetting<int> focus_frame_style { 0 };
    inline static AtomicSetting<int> focus_arrow_style { 0 };
    inline static AtomicSetting<float> focus_visual_seconds { 5.0f };
    inline static AtomicSetting<bool> focus_arrow { true };
    inline static AtomicSetting<bool> focus_distance { true };
    inline static AtomicSetting<bool> randomize_cooldown { true };
    inline static AtomicSetting<int> miss_chance { 0 };
    inline static AtomicSetting<bool> slot_restrict { false };
    inline static AtomicSetting<int> slot_mask { 0x1FF };
    inline static AtomicSetting<float> cooldown_threshold { 95.0f };
    inline static AtomicSetting<float> attack_range { 3.0f };
    inline static AtomicSetting<int> target_type { 0 }; // 0 All, 1 Players, 2 Mobs
};

void                run();
void                init_cache(JNIEnv* env);
void                shutdown_cache();
const jni_cache&    get_cache();

// ── Shutdown flag ────────────────────────────────────────────────────────
// Flipped by the entry layer at DLL unload. Every hook callback checks
// this at its top so we never touch cached JNI state while the cache is
// being torn down on another thread.
inline std::atomic<bool> g_shutting_down{ false };

// Focus-highlight query for ESP integration (returns true if `ent` is the
// currently-locked focus target AND show_focus_visuals is on).
bool is_visual_focus_target(JNIEnv* env, jobject ent);
void render_focus_visual(int display_w, int display_h);

} // namespace vanta::modules::triggerbot
