#pragma once

// SilentAim — aims at a target for the SERVER without moving your camera.
//
// Ported from Vape V4.21's SilentAura. What makes it "silent" is where the
// rotation is applied, not how it is computed:
//
//   Vape wraps the player tick (EventThreadBoundPreTick/PostTick) and writes
//   yaw/pitch into the player for exactly that window, so sendMovementPackets
//   reads the aim rotation while the camera — which renders later, after the
//   fields are restored — never sees it.
//
// This port does the same with the tightest bracket available: a JNIHook on
// ClientPlayerEntity.sendMovementPackets. Apply immediately before, restore
// immediately after. Nothing else in the frame observes the spoofed value.
//
// SAFETY — read this before touching the hook.
// -------------------------------------------
// JNIHook detours by RETRANSFORMING the class and routing the "original"
// through a renamed COPY. On MC 1.21.7+ that copy has been observed to make
// CallNonvirtual* a silent no-op for class_746 — which is why the triggerbot's
// tickMovement hook was removed (the player froze in place). sendMovementPackets
// lives on the same class, and if its original no-ops the player stops sending
// movement entirely: far worse than a visible camera.
//
// So the hook is never trusted blind. install_rotation_hook() first runs a
// PROBE on a harmless class_746 method and only attaches if the copy provably
// executes the original on this host. When the probe fails the module keeps
// working with Rotation=Client and says so in the log. See silentaim.cpp.
//
// Everything else runs from Vanta's GameLoopEvent, which is pumped from the
// wglSwapBuffers detour — in 1.21 that thread IS the game-loop thread, so the
// JNI work lands on the game thread with no bytecode transform anywhere.
//
// Attacks are NOT this module's job, exactly as in Vape (SilentAura delegates
// to SilentAuraClicker). The target is published through
// triggerbot_hook::set_silentaim_target(), which already:
//   • spoofs MinecraftClient.crosshairTarget inside doAttack, so a click — your
//     own or the TriggerBot's — lands on the silent target with a vanilla swing;
//   • suppresses handleBlockBreaking, killing the START_DIGGING packet that
//     otherwise trips Grim's RotationBreak / MultiActionsF when you click at a
//     block while aiming elsewhere.

#include <sdk/mappings/mappings.hpp>
#include <jni.h>
#include <atomic>
#include <functional>

namespace vanta::modules::silentaim {

// What the aura is allowed to lock onto.
enum class Targets : int {
    Players = 0,
    Mobs    = 1,   // every living entity that is NOT a player
    All     = 2,
};

struct jni_cache {
    bool        initialized = false;

    jclass      minecraft_class                 = nullptr;
    jfieldID    mc_player_fid                   = nullptr;
    jfieldID    mc_world_fid                    = nullptr;
    jfieldID    mc_current_screen_fid           = nullptr;

    jclass      client_player_class             = nullptr;

    // ClientPlayerEntity.renderYaw / renderPitch -- the SMOOTHED copy of the
    // view that HeldItemRenderer measures the first-person item's sway against
    // ((getYaw(td) - renderYaw) * 0.1). It chases getYaw() from inside the
    // tick, so it chases the spoofed angle unless we put it back. Names come
    // from mappings.hpp, not from a guess.
    jfieldID    player_render_yaw_fid           = nullptr;
    jfieldID    player_render_pitch_fid         = nullptr;

    jclass      entity_class                    = nullptr;
    jmethodID   entity_get_x_mid                = nullptr;
    jmethodID   entity_get_y_mid                = nullptr;
    jmethodID   entity_get_z_mid                = nullptr;
    // Entity.yaw / Entity.pitch — the REAL rotation fields, not the render
    // interpolated ones. Resolved with a both-ways name fallback so a host
    // running Mojang mappings works without bind_mappings() having been called.
    jfieldID    entity_yaw_fid                  = nullptr;
    jfieldID    entity_pitch_fid                = nullptr;
    jfieldID    entity_prev_yaw_fid             = nullptr;
    jfieldID    entity_prev_pitch_fid           = nullptr;
    jmethodID   entity_is_alive_mid             = nullptr;
    jmethodID   entity_get_eye_height_mid       = nullptr;

    // Entity.isInvisible(). Used to refuse invisible targets — see the note on
    // entity_is_invisible() in silentaim_tick.cpp.
    jmethodID   entity_is_invisible_mid         = nullptr;

    jclass      living_entity_class             = nullptr;
    jmethodID   living_get_health_mid           = nullptr;
    jmethodID   living_is_blocking_mid          = nullptr;

    // Body/head yaw — what actually turns the PLAYER MODEL. These are separate
    // fields from Entity.yaw, and crucially the camera does not read them, so
    // writing them shows the spoofed rotation in third person (F5) while the
    // first-person view stays exactly where the player put it. This is how the
    // rotation becomes verifiable without stopping being silent.
    jfieldID    living_body_yaw_fid             = nullptr;
    jfieldID    living_prev_body_yaw_fid        = nullptr;
    jfieldID    living_head_yaw_fid             = nullptr;
    jfieldID    living_prev_head_yaw_fid        = nullptr;

    jclass      player_entity_class             = nullptr;

    // Held-item read, for Only Weapon.
    jfieldID    player_inventory_fid            = nullptr;
    jfieldID    inventory_selected_slot_fid     = nullptr;
    jmethodID   inventory_get_stack_mid         = nullptr;
    jmethodID   itemstack_is_empty_mid          = nullptr;
    jmethodID   itemstack_get_item_mid          = nullptr;
    jmethodID   item_get_translation_key_mid    = nullptr;

    jclass      world_class                     = nullptr;
    jmethodID   world_get_entities_mid          = nullptr;

    jmethodID   iterable_iterator_mid           = nullptr;
    jmethodID   iterator_has_next_mid           = nullptr;
    jmethodID   iterator_next_mid               = nullptr;

    // Rotation hook plumbing. send_movement_packets_mid is only non-null once
    // the probe has cleared the host — see install_rotation_hook().
    jmethodID   send_movement_packets_mid       = nullptr;

    // Movement correction. Everything here is resolved structurally — the
    // KeyBindings by their translation key, "pressed" as the only boolean
    // field on KeyBinding — because these names move between 1.21.x drops and
    // a wrong guess would silently disable the correction.
    jfieldID    mc_options_fid                  = nullptr;
    jclass      key_binding_class               = nullptr;
    jmethodID   key_binding_translation_mid     = nullptr;
    jfieldID    key_binding_pressed_fid         = nullptr;
    jfieldID    move_key_fids[4]                = { nullptr, nullptr, nullptr, nullptr };
    bool        move_keys_resolved              = false;
    jmethodID   tick_movement_mid               = nullptr;

    // Mouse lattice from the option itself. Mouse.updateMouse computes
    //   d = s*0.6+0.2, f = d*d*d*8, then Entity.changeLookDirection scales the
    // count by 0.15f -- so one mouse count is exactly (float)(8*d^3)*0.15f
    // degrees, in every 1.21.x. Reading the sensitivity is what makes the snap
    // exact from the first tick instead of after a solver has seen 24 mouse
    // windows. smoothCameraEnabled routes the counts through a Smoother, whose
    // output is not on any lattice -- the snap is disabled while it is on.
    jmethodID   options_get_sensitivity_mid     = nullptr;   // class_315.method_42495
    jfieldID    options_mouse_sensitivity_fid   = nullptr;   // class_315.field_1843
    jmethodID   option_get_value_mid            = nullptr;   // class_7172.method_41753
    jfieldID    options_smooth_camera_fid       = nullptr;   // class_315.field_1914
    jmethodID   double_value_mid                = nullptr;   // java/lang/Double.doubleValue

    // Fallback-point guard: the sendMovementPackets point sits AFTER this
    // tick's sprint/sneak packets, so an action there on a toggle tick is
    // PacketOrderF. Both on Entity, both ()Z, both stable across 1.21.x.
    jmethodID   entity_is_sprinting_mid         = nullptr;   // class_1297.method_5624
    jmethodID   entity_is_sneaking_mid          = nullptr;   // class_1297.method_5715

    // Vanilla's own footprints of "already attacked / used an item this
    // tick". doAttack and doItemUse run inside handleInputEvents BEFORE the
    // pre-tick point, so a slot packet or interaction of ours on such a tick
    // would follow vanilla's ATTACK / USE_ITEM (PacketOrderE, PacketOrderJ,
    // MultiActionsF). doItemUse sets itemUseCooldown to 4 (decremented at the
    // top of the next tick); attackEntity resets lastAttackedTicks to 0
    // (incremented in PlayerEntity.tick, i.e. after the pre-tick point). Both
    // ints, same names on 1.21 and 1.21.11.
    jfieldID    mc_item_use_cooldown_fid        = nullptr;   // class_310.field_1752
    jfieldID    player_last_attacked_ticks_fid  = nullptr;   // class_1657.field_6273

    // The wire tracker (see "the rotation on the wire" in silentaim.cpp).
    // sendMovementPackets' own bookkeeping on class_746, READ around the
    // original to learn what actually went out this tick; field_3923 is the
    // only one ever written (the forced flying), and only to 20 -- the value
    // vanilla itself reaches every twentieth stationary tick. Verified on
    // 1.21 and 1.21.11: field_3941 F lastYaw(Client), field_3925 F
    // lastPitch(Client), field_3923 I ticksSinceLastPositionPacketSent,
    // field_3926 / field_3940 / field_3924 D lastX / lastBaseY / lastZ.
    jfieldID    player_last_yaw_fid             = nullptr;   // class_746.field_3941
    jfieldID    player_last_pitch_fid           = nullptr;   // class_746.field_3925
    jfieldID    player_ticks_since_pos_fid      = nullptr;   // class_746.field_3923
    jfieldID    player_last_x_fid               = nullptr;   // class_746.field_3926
    jfieldID    player_last_base_y_fid          = nullptr;   // class_746.field_3940
    jfieldID    player_last_z_fid               = nullptr;   // class_746.field_3924
};

// Four knobs, deliberately. Everything Vape exposes beyond these is pinned to
// its own default inside the controller (see silentaim_tick.cpp): aim speed
// 7.0, target mode Distance, aim area Center, jitter always on. They are part
// of how the motion reads as a hand, not things to tune per-fight.
struct settings {
    inline static std::atomic<bool> enabled       { false };
    inline static std::atomic<bool> overlay_open  { false };   // Vanta ClickGui up

    inline static std::atomic<int>  targets       { 0 };       // Targets

    // Vape: "Max angle", 1 .. 360, default 120. Halved before use, exactly as
    // SilentAura.isValidTarget does (maxAngle / 2).
    inline static std::atomic<int>  max_angle     { 120 };

    // Vape: "Extra swing distance", 0.0 .. 3.0. Added to the 3.0 base reach at
    // which the aura engages.
    inline static std::atomic<int>  extra_reach_x10 { 0 };

    inline static std::atomic<bool> shield_check    { false };

    // Only engage while holding a sword, axe, mace or trident.
    inline static std::atomic<bool> only_weapon     { false };

};

void             init_cache(JNIEnv* env);
void             shutdown_cache();
const jni_cache& get_cache();
jni_cache&       mutable_cache();

void ensure_installed();

// True once the sendMovementPackets hook is attached AND the probe cleared it.
// Rotation=Silent silently degrades to Client while this is false.
bool silent_rotation_available();

// Published by the tick each pump: the rotation the packet should carry, and
// whether it should carry one at all. Read from the hook on the game thread.
// offset_deg is how far that rotation sits from where the player is actually
// looking -- see attack_would_be_unjustified().
void set_managed_rotation(bool active, float yaw, float pitch, float offset_deg);
// ── the action lease ─────────────────────────────────────────────────────────
//
// A block/crystal action (Auto Crystal, Anchor Macro) needs three things the
// pump cannot give it: the rotation has to be on the wire in the SAME flying
// packet that follows the interaction, the interaction has to go out INSIDE the
// tick and before that flying (Grim's Post window starts the moment the flying
// leaves), and the aim has to be verified against the rotation that is actually
// applied, not the one the pump wished for. So the pump only chooses: it leases
// the rotation, hands over a callback, and the hook runs that callback once per
// tick from the point where vanilla itself acts (handleInputEvents), with the
// spoofed yaw/pitch already written into the player.
//
// THE ROTATION ON THE WIRE, and why a block click waits one tick for it.
//
// Grim's RotationPlace runs TWICE per block placement. The post-flying pass
// uses the flying that FOLLOWS the placement (what applied_yaw/pitch is
// validated against); the PRE-flying pass runs the moment the placement
// packet arrives, with the rotation Grim holds at that point -- the PREVIOUS
// flying's -- and tests (yaw,pitch), (lastYaw,pitch), (lastYaw,lastPitch).
// It only runs while the check's flag buffer is up (one post-flying miss
// anywhere sets it to 1, and it decays 0.1 per post-flying HIT), but once it
// is up a failed pre-flying pass is flagged, the placement is DENIED
// server-side, and the post-flying pass that would decay the buffer is
// skipped -- so a module that rotates and clicks in the same tick is denied
// for ever after the first miss. The wire tracker below reports what Grim
// holds, and a block click acts only once Grim's pre-flying pass, run by the
// owner against wire_yaw/pitch and the prev-yaw combos, intercepts the placed
// cell; then a position flying with the applied rotation is forced behind the
// click (when nothing else would go out) so the post-flying pass sees it too.
// Vanilla sends no flying at all on a stationary tick whose rotation did not
// change; without the forced one Grim would judge the click against a later
// flying carrying whatever the lease moved to next.
//
// Cadence of one block click when stationary: tick N the lease lands on the
// aim and the flying carries the LOOK; tick N+1 the same rotation is applied
// again (wire_settled), the pre-flying pass holds, the click goes out and a
// PositionAndOnGround flying with the unchanged rotation follows it; tick N+2
// the next aim steps (look), tick N+3 the next click, and so on. While
// walking the aim angle to the fixed point changes every tick (a Look each
// tick, wire_settled never true): the wire then holds last tick's aim at the
// same point, which still intercepts the cell, so the click goes out on N+1
// all the same -- the owner must gate on the pre-flying pass, not on
// wire_settled. A crystal placement (not a BlockPlace for Grim) and an attack
// are exempt and may act on tick N.
struct ActionContext {
    JNIEnv*  env;            // game thread env, inside a pushed local frame
    jobject  minecraft;      // class_310 (local ref, valid for the call)
    jobject  player;         // class_746 (local ref, valid for the call)
    // Exactly what this tick's flying packet will carry: the stepped angle
    // after the lattice snap and the pitch clamp, already written into
    // Entity.yaw/pitch by the time on_ready runs, and never re-stepped or
    // re-snapped between the callback and the send (see the "applied == sent"
    // note above hkSendMovementPackets). on_tick receives the REAL rotation.
    float    applied_yaw;
    float    applied_pitch;
    bool     position_final; // true only at the sendMovementPackets point (travel already ran)
    bool     frame_open;     // movement-correction frame is open (fields hold the spoof, WASD remapped)
    // Packet-order position of this callback inside the tick.
    //   false: the pre-tick point, inside handleInputEvents -- before
    //          PlayerInput, before the sprint/sneak EntityActions, before the
    //          flying. A slot switch, a slot restore or an interaction here
    //          precedes every EntityAction of the tick (PacketOrderE-safe).
    //   true:  the sendMovementPackets fallback -- this tick's PlayerInput and
    //          sprint/sneak EntityActions have ALREADY gone out. The hook only
    //          dispatches here on ticks where neither sprint nor sneak toggled,
    //          so a slot change is still clean, but callers must not assume
    //          they precede the tick's EntityActions.
    // On either point the hook skips the whole tick (no on_tick, no on_ready)
    // when vanilla itself already attacked or used an item earlier in it, so
    // no callback ever puts a slot packet or an interaction behind a vanilla
    // ATTACK / USE_ITEM in the same tick.
    bool     after_entity_actions;
    unsigned tick_id;        // increments once per client tick
    // The rotation on the wire, i.e. what Grim holds when a packet sent from
    // this callback arrives: the last LOOK that went out (wire_yaw/pitch) and
    // the one before it (wire_prev_*; wire_have_prev is false until two looks
    // have been observed -- treat a missing prev as a pass). wire_settled:
    // applied_yaw/pitch equal wire_yaw/pitch bit for bit, i.e. the rotation
    // this callback validates against was already carried by a flying, so
    // Grim's pre-flying RotationPlace pass evaluates that very angle. Never
    // true on the first tick of a new aim. wire_tick is the tick id of the
    // last flying of any kind.
    float    wire_yaw;
    float    wire_pitch;
    float    wire_prev_yaw;
    float    wire_prev_pitch;
    bool     wire_have_prev;
    bool     wire_settled;
    unsigned wire_tick;
};
enum class ActionResult { Wait, Done, Abort };
struct ActionCallbacks {
    // Once per tick while this owner's lease is live and the rotation has been
    // applied. Done: the packets went out, release after this tick's flying.
    // Wait: nothing sent, keep the lease. Abort: drop the lease now.
    std::function<ActionResult(const ActionContext&)> on_ready;
    // Once per tick ALWAYS (lease or not), before the movement frame opens,
    // so the fields hold the REAL rotation. Used for slot restore and for
    // non-silent actions. May be empty.
    std::function<void(const ActionContext&)>          on_tick;
};

// Leases the rotation for `owner`. Returns whether the lease is HELD by this
// owner (granted or refreshed). It never fails because of the lattice: Grim
// has no GCD check and the snap is defense in depth only. It fails only when
// the verified hook is missing or another owner holds a live lease. `cb` is
// copied on the first grant and kept registered (on_tick keeps running after
// the lease ends) until unregister_action_callbacks(); nullptr only refreshes.
// The lease auto-expires 250 ms after the last refresh and is dropped by the
// hook right after the flying that follows a Done.
bool request_action_rotation(const void* owner, float real_yaw, float real_pitch,
                             float target_yaw, float target_pitch, bool model,
                             const ActionCallbacks* cb = nullptr);
// Immediate release (module disable / gating). The sent rotation eases back to
// the player's view over a few ticks rather than cutting dead.
void release_action_rotation(const void* owner);
// Registers (or replaces) the callbacks without leasing -- on_tick then runs
// every tick, which is how a non-silent action and the slot restore reach the
// tick. Drop them from the owner's destructor.
void register_action_callbacks(const void* owner, const ActionCallbacks& cb);
void unregister_action_callbacks(const void* owner);
// For status text.
bool action_lease_held(const void* owner);
// True while an action rotation is on the wire -- a live lease OR the easing
// that walks it home afterwards. The aura's controller parks on the real view
// while this holds (its publishes are dropped anyway), so the moment the
// easing lands the aura resumes from the player's view and never lands a
// step of its own on top of the action's.
bool action_rotation_active();
// Degrees per mouse count from GameOptions.mouseSensitivity; 0 when unknown or
// while smooth camera is on.
float mouse_lattice_deg();

// True once the pre-tick anchor is live. Without it the spoofed rotation and
// the client's real movement disagree, and Grim's Simulation check sees it.
bool movement_correction_available();

// Called from the triggerbot's handleBlockBreaking hook, which runs inside
// MinecraftClient.handleInputEvents -- i.e. once per tick, BEFORE
// world.tickEntities() drives the player. That is the one phase where the
// movement frame can be opened.
//
// Why borrow that hook instead of installing our own: handleBlockBreaking is
// private AND a leaf. Hooking the dispatcher above it (handleInputEvents)
// attaches fine and then throws inside the original, because JNIHook's class
// copy cannot make the self-calls that method is built out of -- which broke
// attacks. The triggerbot already owns a working detour at the right phase, so
// the correct move is to ride it rather than retransform class_310 twice.
void on_pre_player_tick(JNIEnv* env, jobject minecraft);

// True whenever a spoofed rotation is actually going out on the wire.
//
// Distinct from "a target has been handed to the attack path", and the two must
// not share a flag. The crosshair spoof may only happen once the rotation has
// caught up to the target, but block breaking has to stay suppressed for as
// long as the rotation is spoofed AT ALL -- otherwise vanilla sends
// START_DIGGING for whatever the real crosshair is on while the wire carries an
// angle pointing somewhere else, which is exactly Grim's RotationBreak.
bool rotation_spoof_active();

// True only when an attack right now could NOT be defended to the server.
//
// Deliberately a different question from rotation_spoof_active(). Block
// breaking has to be suppressed for as long as the rotation is spoofed at all,
// because a dig packet names a specific block and the angle has to match it.
// An ATTACK is different: if the rotation we are sending is within a few
// degrees of where the player is actually looking, then whatever they clicked
// is also what the server will raytrace onto, and the hit is honest.
//
// Gating attacks on "is there a spoof" instead of "is the spoof far" is what
// made it impossible to hit anything for the whole half-second of the release,
// and again in the moments before the aim locks -- most of which are windows
// where the sent rotation is nearly the player's own.
bool attack_would_be_unjustified();


inline std::atomic<bool> g_shutting_down{ false };

} // namespace vanta::modules::silentaim
