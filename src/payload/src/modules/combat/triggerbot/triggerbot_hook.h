#pragma once

#include <jni.h>

namespace vanta::modules::triggerbot_hook {

bool init();
void shutdown();
bool is_active();

// Diagnostics — same counters the enhance build shipped, exposed so a
// caller can print them from a debug menu / hotkey.
unsigned long long debug_hook_fire_count();
unsigned long long debug_attack_dispatch_count();
unsigned long long debug_skipped_disabled();
unsigned long long debug_skipped_no_target();
unsigned long long debug_skipped_validation();
unsigned long long debug_skipped_cooldown();
unsigned long long debug_skipped_crit();
unsigned long long debug_skipped_shield();
unsigned long long debug_skipped_using_item();
unsigned long long debug_skipped_post_use();
unsigned long long debug_dispatch_failed();
unsigned long long debug_last_attack_tick_ms();

void notify_disabled();
void on_entity_attacked(JNIEnv* env, jobject target);
unsigned int consume_pending_on_hit();
unsigned int consume_pending_wtap_hit();

// Called from the render thread (wglSwapBuffers) every frame — runs the
// same validation + dispatch path as the world-tick hook but at render
// rate, which drops the bot's reaction latency by ~25-40 ms on average.
void tick_from_render(JNIEnv* env);

// Called from the GAME THREAD once per world tick (20 Hz). Samples
// on_ground + velocity.y for the vertical-state sampler and runs the
// full validation + dispatch path as a fallback if the render-thread
// dispatcher hasn't ticked recently.
void tick_from_movement_packets(JNIEnv* env, jobject local_player);

// True iff `ent` is the entity currently locked by focus_mode.
bool is_focus_target_locked(JNIEnv* env, jobject ent);
// Local reference valid for the current render pass; caller must delete it.
jobject visual_focus_target(JNIEnv* env);

// Optional silent-aim spoof target (nullptr to clear). Not used by the
// standalone triggerbot but kept so a future silent aim module can
// spoof crosshair through hkDoAttack.
void set_silentaim_target(JNIEnv* env, jobject target);

} // namespace vanta::modules::triggerbot_hook
