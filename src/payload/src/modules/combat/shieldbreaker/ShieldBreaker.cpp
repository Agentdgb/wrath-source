#include "ShieldBreaker.h"
#include "ShieldBreakerLogic.h"
#include "core/social/EntityFriends.h"
#include "jvm/JniRef.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include <atomic>
#include <string>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace dt::modules::shieldbreaker {
namespace {
std::atomic<bool> enabled{false};
std::atomic<std::uint32_t> option_bits{(150u << 4) | 15u};
std::atomic<bool> pending_return{false};
std::mutex return_mutex;
struct PendingReturn {
    jobject inventory = nullptr; // global ref, released on return/world change/unload
    jfieldID selected = nullptr;
    int original = -1, axe = -1;
    float remaining_ms = 0;
} pending;

Options options() {
    const auto bits = option_bits.load(std::memory_order_acquire);
    return {bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), int(bits >> 4)};
}
using dt::jvm::LocalObject;
using dt::jvm::LocalString;

bool failed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

std::string item_key(JNIEnv* env, jobject inventory, int slot) {
    const auto& c = triggerbot::get_cache();
    LocalObject stack(env, env->CallObjectMethod(inventory, c.inventory_get_stack_mid, slot));
    if (failed(env) || !stack) return {};
    const auto empty = env->CallBooleanMethod(stack.get(), c.itemstack_is_empty_mid);
    if (failed(env) || empty) return {};
    LocalObject item(env, env->CallObjectMethod(stack.get(), c.itemstack_get_item_mid));
    if (failed(env) || !item) return {};
    LocalString key(env, static_cast<jstring>(env->CallObjectMethod(item.get(), c.item_get_translation_key_mid)));
    if (failed(env) || !key) return {};
    const char* chars = env->GetStringUTFChars(key.get(), nullptr);
    if (!chars) { failed(env); return {}; }
    std::string result(chars);
    env->ReleaseStringUTFChars(key.get(), chars);
    return result;
}

struct Plan {
    jobject inventory = nullptr; // owned local ref
    int original = -1;
    int axe = -1;
};

Plan plan(JNIEnv* env, jobject mc, jobject target, const Options& config) {
    if (!enabled.load(std::memory_order_acquire) || !env || !mc || !target
        || triggerbot::g_shutting_down.load(std::memory_order_acquire)) return {};
    const auto& c = triggerbot::get_cache();
    if (!c.minecraft_player_fid || !c.minecraft_current_screen_fid || !c.player_class
        || !c.player_inventory_fid || !c.inventory_selected_slot_fid
        || !c.inventory_get_stack_mid || !c.itemstack_is_empty_mid
        || !c.itemstack_get_item_mid || !c.item_get_translation_key_mid
        || !c.living_entity_is_blocking_mid || !c.living_entity_is_using_item_mid
        || !c.living_entity_get_health_mid || !c.entity_yaw_fid
        || !c.entity_get_x_mid || !c.entity_get_z_mid) return {};
    LocalObject screen(env, env->GetObjectField(mc, c.minecraft_current_screen_fid));
    if (failed(env) || screen || !env->IsInstanceOf(target, c.player_class)) return {};
    const auto blocking = env->CallBooleanMethod(target, c.living_entity_is_blocking_mid);
    if (failed(env) || !blocking) return {};
    const auto health = env->CallFloatMethod(target, c.living_entity_get_health_mid);
    if (failed(env) || !(health > 0.f) || social::isFriend(env, target)) return {};
    LocalObject player(env, env->GetObjectField(mc, c.minecraft_player_fid));
    if (failed(env) || !player || env->IsSameObject(player.get(), target)) return {};
    const float yaw = env->GetFloatField(target, c.entity_yaw_fid);
    if (failed(env)) return {};
    const double px = env->CallDoubleMethod(player.get(), c.entity_get_x_mid);
    if (failed(env)) return {};
    const double pz = env->CallDoubleMethod(player.get(), c.entity_get_z_mid);
    if (failed(env)) return {};
    const double tx = env->CallDoubleMethod(target, c.entity_get_x_mid);
    if (failed(env)) return {};
    const double tz = env->CallDoubleMethod(target, c.entity_get_z_mid);
    if (failed(env) || (config.check_facing && !shield_faces_attacker(yaw, px - tx, pz - tz))) return {};
    const auto using_item = env->CallBooleanMethod(player.get(), c.living_entity_is_using_item_mid);
    if (failed(env) || using_item) return {};
    LocalObject inventory(env, env->GetObjectField(player.get(), c.player_inventory_fid));
    if (failed(env) || !inventory) return {};
    const int original = env->GetIntField(inventory.get(), c.inventory_selected_slot_fid);
    if (failed(env) || original < 0 || original > 8) return {};
    const auto held = item_key(env, inventory.get(), original);
    if (axe_rank(held) >= 0) return {inventory.release(), original, original};
    if (config.only_swords && !is_sword(held)) return {};
    int best = -1, rank = -1;
    for (int slot = 0; slot < 9; ++slot) {
        const int candidate = axe_rank(item_key(env, inventory.get(), slot));
        if (candidate > rank) { best = slot; rank = candidate; }
        if (!config.strongest_axe && best >= 0) break;
    }
    if (best < 0) return {};
    return {inventory.release(), original, best};
}
}

bool can_break(JNIEnv* env, jobject mc, jobject target) {
    const auto p = plan(env, mc, target, options());
    if (p.inventory) env->DeleteLocalRef(p.inventory);
    return p.axe >= 0;
}

void configure(Options config) noexcept {
    const auto bits = (std::uint32_t(std::clamp(config.return_delay_ms, 0, 500)) << 4)
        | (config.only_swords ? 1u : 0u) | (config.switch_back ? 2u : 0u)
        | (config.check_facing ? 4u : 0u) | (config.strongest_axe ? 8u : 0u);
    option_bits.store(bits, std::memory_order_release);
}

bool has_pending_return() noexcept { return pending_return.load(std::memory_order_acquire); }

// Caller holds return_mutex. Only JNI field accesses here; no Java callbacks.
static void finish_return(JNIEnv* env, bool restore) {
    if (pending.inventory) {
        const int current = env->GetIntField(pending.inventory, pending.selected);
        if (!failed(env) && restore && should_restore(current, pending.axe, pending.original)) {
            env->SetIntField(pending.inventory, pending.selected, pending.original);
            failed(env);
        }
        env->DeleteGlobalRef(pending.inventory);
    }
    pending = {};
    pending_return.store(false, std::memory_order_release);
}

void clear_pending_return(JNIEnv* env, bool restore) {
    if (!env) return;
    std::lock_guard lock(return_mutex);
    finish_return(env, restore);
}

void pump_return(JNIEnv* env, jobject mc, float delta_ms) {
    if (!env || !has_pending_return()) return;
    const auto& c = triggerbot::get_cache();
    LocalObject player(env, mc && c.minecraft_player_fid ? env->GetObjectField(mc, c.minecraft_player_fid) : nullptr);
    if (failed(env)) { clear_pending_return(env, false); return; }
    LocalObject inventory(env, player && c.player_inventory_fid ? env->GetObjectField(player.get(), c.player_inventory_fid) : nullptr);
    if (failed(env)) { clear_pending_return(env, false); return; }
    std::lock_guard lock(return_mutex);
    if (!pending.inventory) return;
    if (!inventory || !env->IsSameObject(inventory.get(), pending.inventory)) {
        finish_return(env, false); // disconnected, respawned or changed worlds
        return;
    }
    const int current = env->GetIntField(inventory.get(), pending.selected);
    if (failed(env) || current != pending.axe) { finish_return(env, false); return; }
    if (std::isfinite(delta_ms) && delta_ms > 0.f) pending.remaining_ms -= std::min(delta_ms, 1000.f);
    if (pending.remaining_ms <= 0.f || !enabled.load(std::memory_order_acquire)) finish_return(env, true);
}

void set_enabled(bool value) noexcept {
    enabled.store(value, std::memory_order_release);
}

AttackSlot::AttackSlot(JNIEnv* env, jobject mc) : env_(env), options_(options()) {
    if (!enabled.load(std::memory_order_acquire) || !env || !mc) return;
    const auto& c = triggerbot::get_cache();
    if (!c.minecraft_crosshair_target_fid || !c.entity_hit_result_class
        || !c.entity_hit_result_get_entity_mid) return;
    LocalObject hit(env, env->GetObjectField(mc, c.minecraft_crosshair_target_fid));
    if (failed(env) || !hit || !env->IsInstanceOf(hit.get(), c.entity_hit_result_class)) return;
    LocalObject target(env, env->CallObjectMethod(hit.get(), c.entity_hit_result_get_entity_mid));
    if (failed(env) || !target) return;
    const auto p = plan(env, mc, target.get(), options_);
    inventory_ = p.inventory;
    original_ = p.original;
    axe_ = p.axe;
    selected_ = c.inventory_selected_slot_fid;
    if (inventory_ && original_ != axe_) {
        // Vanilla attackEntity synchronizes the selected slot before its attack.
        env->SetIntField(inventory_, selected_, axe_);
        failed(env);
    }
}

AttackSlot::~AttackSlot() {
    if (!inventory_) return;
    // Preserve an exception from the original attack while cleaning up.
    jthrowable pending_exception = env_->ExceptionOccurred();
    if (pending_exception) env_->ExceptionClear();
    const int current = env_->GetIntField(inventory_, selected_);
    if (!failed(env_) && should_restore(current, axe_, original_)) {
        const bool success = completed_ && !pending_exception;
        bool deferred = success && !options_.switch_back;
        if (success && options_.switch_back && options_.return_delay_ms > 0) {
            std::lock_guard lock(return_mutex);
            auto& queued = pending;
            // Never replace an existing transaction's original slot.
            if (!queued.inventory) {
                queued.inventory = env_->NewGlobalRef(inventory_);
                if (!failed(env_) && queued.inventory) {
                    queued.selected = selected_;
                    queued.original = original_;
                    queued.axe = axe_;
                    queued.remaining_ms = static_cast<float>(options_.return_delay_ms);
                    pending_return.store(true, std::memory_order_release);
                    deferred = true;
                }
            }
        }
        if (!deferred) {
            env_->SetIntField(inventory_, selected_, original_);
            failed(env_);
        }
    }
    env_->DeleteLocalRef(inventory_);
    if (pending_exception) { env_->Throw(pending_exception); env_->DeleteLocalRef(pending_exception); }
}

}
