#pragma once
// antiaction.h -- the gates Anti Action exposes to the hooks that own the
// action sites, plus the one seam another module can borrow on the use site.
//
//   should_block_attack -- asked by the triggerbot's class_310.doAttack detour
//                          before it lets the original run. True means "eat the
//                          click": no attack, no swing.
//   should_block_use    -- asked by this module's own class_310.doItemUse detour
//                          (or, when that hook could not be installed, by the
//                          frame pump that drives the window-level fallback).
//
// Both run on the game thread at the moment of the action, read only client
// state (crosshair target, main hand, the block under the crosshair), never
// throw and always leave the JNI env without a pending exception. When the
// module is disabled they answer false without touching the JVM.
//
//   use observer        -- one optional callback the doItemUse detour asks
//                          BEFORE its own rules, with the click not yet acted
//                          on. Returning true consumes the click: no original,
//                          no packet, no swing, and Anti Action's rules are not
//                          consulted for it either. This is how Safe Anchor
//                          takes over the player's own right-click on an
//                          anchor. The detour is installed while an observer is
//                          registered even when Anti Action itself is off, and
//                          the observer is only asked once the hook's probe has
//                          proven the original runs on this host.

#include <jni.h>

namespace dt::modules::antiaction {

bool should_block_attack(JNIEnv* env, jobject mc);
bool should_block_use(JNIEnv* env, jobject mc);

// Runs on the game thread inside MinecraftClient.doItemUse. `mc` is the
// client (local ref, valid for the call). Must never throw and must leave the
// env without a pending exception.
using UseObserver = bool (*)(JNIEnv* env, jobject mc, void* user);

// Registers (or, with nullptr, clears) the observer. Only one at a time; a
// second registration replaces the first. Safe from any thread.
void set_use_observer(UseObserver fn, void* user);

// True while the doItemUse detour is attached AND its probe passed, i.e. the
// observer actually sees every right-click. While false a client that needs
// the click has to watch the use key itself.
bool use_hook_live();

} // namespace dt::modules::antiaction
