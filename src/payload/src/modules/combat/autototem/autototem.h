#pragma once

// AutoTotem — keeps a Totem of Undying in the off-hand.
//
// Three modes, in ascending order of how much noise they make on the wire:
//
//   Hotbar    — totem lives in slots 0-8. Selects its slot and lets vanilla's
//               own F keybind emit the swap. Two packets, both from vanilla
//               call sites, and ZERO inventory traffic. Because it never sends
//               a ClickSlot, Grim's MultiActions family — which is entirely
//               about inventory actions — has nothing to look at. This is the
//               quiet path; prefer it whenever the totem can live in the bar.
//
//   Inventory — totem lives in the main inventory, out of the F key's reach.
//               Needs the player screen and a real ClickSlot, which IS the
//               surface the inventory-side checks watch.
//
//   Silent    — same click, but no screen is ever shown. Movement is
//               suppressed for the session, since a real screen stops movement
//               input, and a CloseHandledScreen ends it.
//
// Runs from Dt's GameLoopEvent, throttled to 20 Hz. In MC 1.21 the render
// pump thread IS the thread that runs the game loop, so the JNI work lands on
// the game thread without retransforming any bytecode.

#include <sdk/mappings/mappings.hpp>
#include <jni.h>
#include <atomic>

namespace dt::modules::autototem {

enum class Mode : int {
    Hotbar    = 0,
    Inventory = 1,
    Silent    = 2,
};

// Inventory mode only: who opens the screen.
enum class OpenMode : int {
    AutoOpen = 0,   // the module opens (and closes) it around the swap
    OnOpen   = 1,   // only acts while YOU already have it open
};

struct jni_cache {
    bool        initialized = false;

    jclass      minecraft_class                 = nullptr;
    jfieldID    mc_player_fid                   = nullptr;
    jfieldID    mc_current_screen_fid           = nullptr;
    jfieldID    mc_interaction_manager_fid      = nullptr;
    jfieldID    mc_options_fid                  = nullptr;
    jfieldID    mc_crosshair_target_fid         = nullptr;
    jmethodID   mc_set_screen_mid               = nullptr;

    jclass      client_player_class             = nullptr;
    jfieldID    player_network_handler_fid      = nullptr;

    jclass      player_entity_class             = nullptr;
    jfieldID    player_inventory_fid            = nullptr;

    jclass      living_entity_class             = nullptr;
    jmethodID   get_main_hand_stack_mid         = nullptr;
    jmethodID   get_off_hand_stack_mid          = nullptr;

    jclass      inventory_class                 = nullptr;
    jfieldID    inventory_selected_slot_fid     = nullptr;
    jmethodID   inventory_get_stack_mid         = nullptr;

    jclass      itemstack_class                 = nullptr;
    jmethodID   itemstack_is_empty_mid          = nullptr;
    jmethodID   itemstack_get_item_mid          = nullptr;

    jclass      item_class                      = nullptr;
    jmethodID   item_get_translation_key_mid    = nullptr;

    jclass      interaction_manager_class       = nullptr;
    jmethodID   sync_selected_slot_mid          = nullptr;
    jmethodID   click_slot_mid                  = nullptr;
    jobject     slot_action_swap_ref            = nullptr;   // global ref

    jclass      inventory_screen_class          = nullptr;
    jmethodID   inventory_screen_ctor           = nullptr;

    jclass      network_handler_class           = nullptr;
    jmethodID   send_packet_mid                 = nullptr;

    // PlayerActionC2SPacket(Action, BlockPos, Direction) — the direct-send
    // fallback for Hotbar mode when the vanilla keybind route is unavailable.
    jclass      player_action_packet_class      = nullptr;
    jmethodID   player_action_packet_ctor       = nullptr;
    jobject     swap_offhand_action_ref         = nullptr;   // global ref
    jobject     block_pos_origin_ref            = nullptr;   // global ref
    jobject     direction_down_ref              = nullptr;   // global ref

    // CloseHandledScreenC2SPacket(syncId) — Silent mode's session close.
    jclass      close_screen_packet_class       = nullptr;
    jmethodID   close_screen_packet_ctor        = nullptr;

    // Digging gate: a swap is a PlayerAction, same family as the block-break
    // packets, so we never slip one into a live break.
    jclass      game_options_class              = nullptr;
    jfieldID    options_attack_key_fid          = nullptr;
    jclass      block_hit_result_class          = nullptr;

    // KeyBinding plumbing. Everything below is resolved by translation key or
    // by shape rather than by intermediary name — those move between 1.21.x
    // drops and several are flagged "best-guess" in mappings.hpp.
    jclass      key_binding_class               = nullptr;
    jmethodID   key_binding_get_translation_mid = nullptr;
    jmethodID   key_binding_is_pressed_mid      = nullptr;
    jmethodID   key_binding_set_pressed_mid     = nullptr;
    jfieldID    key_binding_times_pressed_fid   = nullptr;
    jfieldID    key_binding_pressed_fid         = nullptr;

    // Resolved lazily on the first pump with a live GameOptions.
    jfieldID    options_swap_hands_key_fid      = nullptr;
    jfieldID    movement_key_fids[6]            = { nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr };
    int         movement_key_count              = 0;
    bool        keybinds_resolved               = false;
    bool        keybind_route_usable            = false;
};

struct settings {
    inline static std::atomic<bool> enabled         { false };
    inline static std::atomic<bool> overlay_open    { false };  // Dt ClickGui up

    inline static std::atomic<int>  mode            { 0 };      // Mode
    inline static std::atomic<int>  open_mode       { 0 };      // OpenMode
    inline static std::atomic<bool> auto_close      { false };  // OnOpen only
    inline static std::atomic<bool> return_original { true  };  // Hotbar only

    // Reaction delay, drawn FLAT (uniform) across [min, max] every cycle.
    // Flat, not bell-shaped and not a fast/slow mixture: a mixture leaves a
    // sparse slow tail that drags the mean up, so nearly every sample sits
    // below it — the "low outliers" signature. A flat draw has neither a tail
    // nor a modal peak, and over [60, 660] gives sigma ≈ 173 ms.
    inline static std::atomic<int>  reaction_min_ms { 60 };
    inline static std::atomic<int>  reaction_max_ms { 660 };

    // Master switch: flat random reaction + random totem pick. Off = fixed
    // midpoint and the leftmost totem, which any consistency check reads
    // immediately.
    inline static std::atomic<bool> randomization   { true };

    // Ticks between selecting the totem and sending the swap. The pump adds
    // 0-2 random ticks on top per cycle; that spread is what stops consecutive
    // cycles landing on the same total wire time.
    inline static std::atomic<int>  equip_delay     { 1 };

    // Silent mode: how long movement stays suppressed after the session ends.
    inline static std::atomic<int>  silent_move_min_ms { 100 };
    inline static std::atomic<int>  silent_move_max_ms { 200 };
};

void             init_cache(JNIEnv* env);
void             shutdown_cache();
const jni_cache& get_cache();
jni_cache&       mutable_cache();

void ensure_installed();

inline std::atomic<bool> g_shutting_down{ false };

} // namespace dt::modules::autototem
