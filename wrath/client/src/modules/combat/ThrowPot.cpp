#include "modules/combat/ThrowPotPlan.h"
#include "modules/combat/ThrowPotTiming.h"
#include "modules/combat/silentaim/silentaim.h"
// ThrowPot — one key press throws healing, strength or speed splash potions
// from the hotbar. Every press is a bounded action: it either throws or
// cancels, with tick-scheduled phases and a bounded healing plan.
//
// The source supplied for this module targeted an older wrapper API and the
// legacy numeric potion metadata.  Vanta supports Minecraft 1.21, where potion
// effects live in components, so the implementation below identifies the
// stack through its translatable display-name key instead. Every action keeps
// the safe select, use, restore ordering, including multi-potion sequences.

#include "core/UnloadSafety.h"
#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/NumericSetting.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"

#include "util/Log.h"
#include "vgui/vgui.h"

#include <sdk/mappings/mappings.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <jni.h>
#include <mutex>
#include <string>
#include <vector>

namespace vanta::modules {

namespace {

struct CacheData {
    jclass mc_class = nullptr;
    jfieldID mc_instance = nullptr;
    jfieldID mc_player = nullptr;
    jfieldID mc_screen = nullptr;
    jfieldID mc_interaction = nullptr;

    jfieldID player_inventory = nullptr;
    jmethodID living_health = nullptr;
    jmethodID living_max_health = nullptr;
    jmethodID living_using_item = nullptr;
    jmethodID living_swing_hand = nullptr;
    jmethodID entity_has_vehicle = nullptr;

    jfieldID inventory_selected = nullptr;
    jmethodID inventory_get_stack = nullptr;

    jmethodID stack_is_empty = nullptr;
    jmethodID stack_translation = nullptr;
    jmethodID stack_get_name = nullptr;

    jmethodID text_get_content = nullptr;
    jmethodID text_get_string = nullptr;
    jclass translatable_class = nullptr;
    jmethodID translatable_key = nullptr;

    jmethodID sync_selected_slot = nullptr;
    jmethodID interaction_use_item = nullptr;
    jmethodID result_is_accepted = nullptr;
    jobject main_hand = nullptr;
};

struct Cache : CacheData {
    std::atomic<bool> tried{false};
    std::atomic<bool> ready{false};
};

Cache g_cache;
// Leaf lock: only transfer plain cache data here. No JNI, logging, event
// dispatch or acquisition of clientStateMutex while this mutex is held.
static std::mutex s_cache_mx;

void release_cache(JNIEnv *env, CacheData &cache) {
    if (!env)
        return;
    if (cache.mc_class)
        env->DeleteGlobalRef(cache.mc_class);
    if (cache.translatable_class)
        env->DeleteGlobalRef(cache.translatable_class);
    if (cache.main_hand)
        env->DeleteGlobalRef(cache.main_hand);
    cache = {};
}

bool clear_exception(JNIEnv *env) {
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionClear();
    return true;
}

jclass global_class(JNIEnv *env, const char *name) {
    if (!name)
        return nullptr;
    jclass local = vanta::jvm::findClassAnyLoader(env, name);
    if (clear_exception(env) || !local)
        return nullptr;
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

jmethodID method_either(JNIEnv *env, jclass cls, const char *yarn, const char *mojang, const char *sig) {
    if (!cls)
        return nullptr;
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    for (const char *name : {mojang_first ? mojang : yarn, mojang_first ? yarn : mojang}) {
        jmethodID method = env->GetMethodID(cls, name, sig);
        clear_exception(env);
        if (method)
            return method;
    }
    return nullptr;
}

jfieldID field_either(JNIEnv *env, jclass cls, const char *yarn, const char *mojang, const char *sig) {
    if (!cls)
        return nullptr;
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    for (const char *name : {mojang_first ? mojang : yarn, mojang_first ? yarn : mojang}) {
        jfieldID fid = env->GetFieldID(cls, name, sig);
        clear_exception(env);
        if (fid)
            return fid;
    }
    return nullptr;
}

void resolve_cache(JNIEnv *env, Cache &cache) {
    using namespace sdk::mappings;

    cache.mc_class = global_class(env, minecraftclass_sig);
    if (!cache.mc_class)
        return;

    cache.mc_instance = env->GetStaticFieldID(cache.mc_class, minecraftclient_name, minecraftclient_sig);
    clear_exception(env);
    cache.mc_player = env->GetFieldID(cache.mc_class, player_name, player_sig);
    clear_exception(env);
    cache.mc_screen = env->GetFieldID(cache.mc_class, minecraft_current_screen_name, minecraft_current_screen_sig);
    clear_exception(env);
    cache.mc_interaction = env->GetFieldID(cache.mc_class, interaction_manager_name, interaction_manager_sig);
    clear_exception(env);

    if (jclass player_class = global_class(env, player_entity_class_sig)) {
        cache.player_inventory = env->GetFieldID(player_class, player_inventory_name, player_inventory_sig);
        clear_exception(env);
        env->DeleteGlobalRef(player_class);
    }

    if (jclass living_class = global_class(env, living_entity_class_sig)) {
        cache.living_health =
            env->GetMethodID(living_class, living_entity_get_health_name, living_entity_get_health_sig);
        clear_exception(env);
        cache.living_max_health =
            env->GetMethodID(living_class, living_entity_get_max_health_name, living_entity_get_max_health_sig);
        clear_exception(env);
        cache.living_using_item =
            env->GetMethodID(living_class, living_entity_is_using_item_name, living_entity_is_using_item_sig);
        clear_exception(env);
        cache.living_swing_hand = env->GetMethodID(living_class, swing_hand_name, swing_hand_sig);
        clear_exception(env);
        env->DeleteGlobalRef(living_class);
    }

    if (jclass entity_class = global_class(env, entity_class_sig)) {
        cache.entity_has_vehicle = method_either(env, entity_class, "method_5765", "isPassenger", "()Z");

        env->DeleteGlobalRef(entity_class);
    }

    if (jclass inventory_class = global_class(env, player_inventory_class_sig)) {
        cache.inventory_selected =
            env->GetFieldID(inventory_class, inventory_selected_slot_name, inventory_selected_slot_sig);
        clear_exception(env);
        cache.inventory_get_stack =
            env->GetMethodID(inventory_class, inventory_get_stack_name, inventory_get_stack_sig);
        clear_exception(env);
        env->DeleteGlobalRef(inventory_class);
    }

    if (jclass stack_class = global_class(env, itemstack_class_sig)) {
        cache.stack_is_empty = env->GetMethodID(stack_class, itemstack_is_empty_name, itemstack_is_empty_sig);
        clear_exception(env);
        cache.stack_translation =
            method_either(env, stack_class, "method_7922", "getDescriptionId", "()Ljava/lang/String;");
        cache.stack_get_name = env->GetMethodID(stack_class, itemstack_get_name_name, itemstack_get_name_sig);
        clear_exception(env);
        env->DeleteGlobalRef(stack_class);
    }

    if (jclass text_class = global_class(env, text_class_sig)) {
        const char *content_sig =
            is_mojang_layout ? "()Lnet/minecraft/network/chat/ComponentContents;" : "()Lnet/minecraft/class_7417;";
        cache.text_get_content = method_either(env, text_class, "method_10851", "getContents", content_sig);
        // Component implements Brigadier's Message; this method is stable even
        // when Minecraft's own names are obfuscated.
        cache.text_get_string = env->GetMethodID(text_class, "getString", "()Ljava/lang/String;");
        clear_exception(env);
        env->DeleteGlobalRef(text_class);
    }

    const char *translatable_name =
        is_mojang_layout ? "net/minecraft/network/chat/contents/TranslatableContents" : "net/minecraft/class_2588";
    cache.translatable_class = global_class(env, translatable_name);
    if (cache.translatable_class) {
        cache.translatable_key =
            method_either(env, cache.translatable_class, "method_11022", "getKey", "()Ljava/lang/String;");
    }

    if (jclass interaction_class = global_class(env, interaction_manager_class_sig)) {
        // Calling MinecraftClient.startUseItem would first attempt the block
        // under the crosshair.  That produces an unnecessary INTERACT_BLOCK
        // before a potion throw and Matrix reports interact.vis when its
        // tracked block differs from the client's ray.  Use the vanilla
        // interaction manager's item-only path instead; it still creates the
        // correct sequenced USE_ITEM packet for the running protocol.
        for (const char *name : {interact_item_name, "method_2919", "useItem"}) {
            cache.interaction_use_item = env->GetMethodID(interaction_class, name, interact_item_sig);
            clear_exception(env);
            if (cache.interaction_use_item)
                break;
        }

        // Mojang has renamed this private helper across 1.21 patch lines.
        // Keep the mapping-selected spelling first, then the known runtime
        // spellings; all have the same no-argument shape.
        for (const char *name :
             {sync_selected_slot_name, "method_2911", "ensureHasSentCarriedItem", "broadcastCarriedItem"}) {
            cache.sync_selected_slot = env->GetMethodID(interaction_class, name, sync_selected_slot_sig);
            clear_exception(env);
            if (cache.sync_selected_slot)
                break;
        }
        env->DeleteGlobalRef(interaction_class);
    }

    if (jclass hand_class = global_class(env, hand_class_sig)) {
        jfieldID main_hand_field = env->GetStaticFieldID(hand_class, hand_main_hand_name, hand_main_hand_sig);
        clear_exception(env);
        if (main_hand_field) {
            jobject local = env->GetStaticObjectField(hand_class, main_hand_field);
            if (!clear_exception(env) && local) {
                cache.main_hand = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
        env->DeleteGlobalRef(hand_class);
    }

    const char *result_class_name =
        is_mojang_layout ? "net/minecraft/world/InteractionResult" : "net/minecraft/class_1269";
    if (jclass result_class = global_class(env, result_class_name)) {
        cache.result_is_accepted = method_either(env, result_class, "method_23665", "consumesAction", "()Z");
        env->DeleteGlobalRef(result_class);
    }

    const bool identity_ready =
        cache.stack_translation || (cache.stack_get_name && cache.text_get_string) ||
        (cache.stack_get_name && cache.text_get_content && cache.translatable_class && cache.translatable_key);

    cache.ready = cache.mc_instance && cache.mc_player && cache.mc_screen && cache.mc_interaction &&
                  cache.interaction_use_item && cache.player_inventory && cache.living_health &&
                  cache.living_max_health && cache.living_using_item && cache.living_swing_hand &&
                  cache.entity_has_vehicle && cache.main_hand && cache.result_is_accepted && cache.inventory_selected &&
                  cache.inventory_get_stack && cache.stack_is_empty && identity_ready && cache.sync_selected_slot;

    VLOG_INFO("throwpot",
              "cache: mc={} player={} health={}/{} using={} vehicle={} inventory={} stack={} "
              "identity={} sync={} itemUse={} hand={} swing={} -> {}",
              cache.mc_instance ? 1 : 0, cache.mc_player ? 1 : 0, cache.living_health ? 1 : 0,
              cache.living_max_health ? 1 : 0, cache.living_using_item ? 1 : 0, cache.entity_has_vehicle ? 1 : 0,
              (cache.player_inventory && cache.inventory_selected && cache.inventory_get_stack) ? 1 : 0,
              cache.stack_is_empty ? 1 : 0, identity_ready ? 1 : 0, cache.sync_selected_slot ? 1 : 0,
              cache.interaction_use_item ? 1 : 0, cache.main_hand ? 1 : 0, cache.living_swing_hand ? 1 : 0,
              cache.ready ? "ready" : "unusable");
}

void build_cache(JNIEnv *env) {
    // Serialize resolution/destruction with callbacks, without holding the
    // leaf mutex across JNI calls that can enter Java or another subsystem.
    std::lock_guard state(clientStateMutex());
    if (g_cache.tried.load(std::memory_order_acquire))
        return;
    g_cache.tried.store(true, std::memory_order_release);
    Cache candidate;
    try {
        resolve_cache(env, candidate);
    } catch (...) {
        release_cache(env, candidate);
        g_cache.tried.store(false, std::memory_order_release);
        throw;
    }
    {
        std::lock_guard leaf(s_cache_mx);
        static_cast<CacheData &>(g_cache) = static_cast<CacheData &>(candidate);
        g_cache.ready.store(candidate.ready.load(std::memory_order_acquire), std::memory_order_release);
        g_cache.tried.store(true, std::memory_order_release);
    }
}

void append_java_string(JNIEnv *env, std::string &out, jstring value) {
    if (!value)
        return;
    const char *text = env->GetStringUTFChars(value, nullptr);
    if (!text) {
        clear_exception(env);
        return;
    }
    if (!out.empty())
        out.push_back('|');
    out.append(text);
    env->ReleaseStringUTFChars(value, text);
}

std::string stack_identity(JNIEnv *env, jobject stack) {
    std::string identity;

    if (g_cache.stack_translation) {
        auto key = static_cast<jstring>(env->CallObjectMethod(stack, g_cache.stack_translation));
        if (clear_exception(env))
            key = nullptr;
        if (key) {
            append_java_string(env, identity, key);
            env->DeleteLocalRef(key);
        }
    }

    // A known non-potion item cannot become a splash potion through a custom
    // display name. Avoid the extra component/string lookups for these slots.
    if (!identity.empty() && identity.find("splash_potion") == std::string::npos)
        return identity;

    if (!g_cache.stack_get_name)
        return identity;
    jobject name = env->CallObjectMethod(stack, g_cache.stack_get_name);
    if (clear_exception(env) || !name)
        return identity;

    bool got_translation_key = false;
    if (g_cache.text_get_content && g_cache.translatable_class && g_cache.translatable_key) {
        jobject content = env->CallObjectMethod(name, g_cache.text_get_content);
        if (clear_exception(env))
            content = nullptr;
        if (content) {
            if (env->IsInstanceOf(content, g_cache.translatable_class) == JNI_TRUE) {
                auto key = static_cast<jstring>(env->CallObjectMethod(content, g_cache.translatable_key));
                if (clear_exception(env))
                    key = nullptr;
                if (key) {
                    append_java_string(env, identity, key);
                    env->DeleteLocalRef(key);
                    got_translation_key = true;
                }
            }
            env->DeleteLocalRef(content);
        }
    }

    // English-name fallback for unusual clients where the translatable content
    // accessor moved.  The language-independent key above is preferred.
    if (!got_translation_key && g_cache.text_get_string) {
        auto text = static_cast<jstring>(env->CallObjectMethod(name, g_cache.text_get_string));
        if (clear_exception(env))
            text = nullptr;
        if (text) {
            append_java_string(env, identity, text);
            env->DeleteLocalRef(text);
        }
    }

    env->DeleteLocalRef(name);
    return identity;
}

enum class PotionKind { Healing, Strength, Speed };

const char *potion_name(PotionKind kind) {
    switch (kind) {
    case PotionKind::Healing:
        return "healing";
    case PotionKind::Strength:
        return "strength";
    case PotionKind::Speed:
        return "speed";
    }
    return "unknown";
}

bool identity_matches_potion(std::string identity, PotionKind kind) {
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const bool splash =
        identity.find("splash_potion") != std::string::npos || identity.find("splash potion") != std::string::npos;
    if (!splash)
        return false;

    switch (kind) {
    case PotionKind::Healing:
        return identity.find("healing") != std::string::npos || identity.find("instant_health") != std::string::npos ||
               identity.find("instant health") != std::string::npos;
    case PotionKind::Strength:
        return identity.find("strength") != std::string::npos;
    case PotionKind::Speed:
        // Minecraft calls the potion "Swiftness", while the underlying
        // status effect and some clients call it "Speed".
        return identity.find("swiftness") != std::string::npos || identity.find("effect.speed") != std::string::npos ||
               identity.find("potion.speed") != std::string::npos;
    }
    return false;
}

int healing_points_from_identity(std::string identity) {
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Instant Health I heals 4 health points; level II heals 8. The potion
    // name/component key normally exposes the strong_ tier. Keep display-name
    // fallbacks for clients that flatten the component into text.
    if (identity.find("strong_healing") != std::string::npos || identity.find("healing_ii") != std::string::npos ||
        identity.find("healing ii") != std::string::npos) {
        return 8;
    }
    return 4;
}

bool is_splash_potion(JNIEnv *env, jobject inventory, int slot, PotionKind kind, int *healing_points = nullptr) {
    jobject stack = env->CallObjectMethod(inventory, g_cache.inventory_get_stack, static_cast<jint>(slot));
    if (clear_exception(env) || !stack)
        return false;

    bool result = false;
    const jboolean empty = env->CallBooleanMethod(stack, g_cache.stack_is_empty);
    if (!clear_exception(env) && empty != JNI_TRUE) {
        const std::string identity = stack_identity(env, stack);
        result = identity_matches_potion(identity, kind);
        if (result && healing_points && kind == PotionKind::Healing)
            *healing_points = healing_points_from_identity(identity);
    }

    env->DeleteLocalRef(stack);
    return result;
}

bool right_mouse_down() { return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0; }

bool minecraft_has_focus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    return process == GetCurrentProcessId();
}

bool player_is_using(JNIEnv *env, jobject player) {
    const jboolean using_item = env->CallBooleanMethod(player, g_cache.living_using_item);
    return !clear_exception(env) && using_item == JNI_TRUE;
}

bool select_slot(JNIEnv *env, jobject interaction, jobject inventory, int slot, bool *changed = nullptr) {
    if (changed)
        *changed = false;
    const jint current = env->GetIntField(inventory, g_cache.inventory_selected);
    if (clear_exception(env))
        return false;
    // Sending a second HELD_ITEM_CHANGE for the already-selected slot is an
    // explicit BadPackets violation in Grim.  A no-op selection needs no wire
    // packet at all.
    if (current == slot)
        return true;

    env->SetIntField(inventory, g_cache.inventory_selected, static_cast<jint>(slot));
    if (clear_exception(env))
        return false;
    env->CallVoidMethod(interaction, g_cache.sync_selected_slot);
    if (clear_exception(env))
        return false;
    if (changed)
        *changed = true;
    return true;
}

} // namespace

// Preserve profiles written before the existing mode was relabelled.
class ThrowPotThrowsSetting final : public EnumSetting {
  public:
    explicit ThrowPotThrowsSetting(Module *owner) : EnumSetting(owner, "Throws", {"Randomization", "Double Pot"}, 0) {}

    void load(const nlohmann::json &value) override {
        if (value.is_string() && value == "Randomize")
            set(0);
        else
            EnumSetting::load(value);
    }
};

class ThrowPot final : public Module {
  public:
    ThrowPot()
        : Module("ThrowPot", "Throws healing, strength or speed pots with configurable timing", Category::Combat) {
        registerSetting(&potion_);
        registerSetting(&throws_);
        registerSetting(&cooldown_);
        registerSetting(&safe_mode_);

        potion_.setDescription("Potion type to find in the hotbar and throw.");
        throws_.setDescription("Throws up to two potions, or one if it already completes healing.");
        cooldown_.setDescription("Potion cooldown: 1 = 50 ms (fastest), 10 = 500 ms. Select and throw in one input tick; restore on a later tick.");
        safe_mode_.setDescription("Prevents throwing extra potions when health is full.");

        LARGE_INTEGER seed{};
        QueryPerformanceCounter(&seed);
        random_state_ ^= static_cast<std::uint32_t>(seed.QuadPart) ^ GetCurrentProcessId();
        if (!random_state_)
            random_state_ = 1;
        keybind().setMode(BindMode::Once);
    }

    ~ThrowPot() override {
        silentaim::unregister_action_callbacks(this);
        std::lock_guard state(clientStateMutex());
        vanta::jvm::JniScope scope;
        if (!scope && (g_cache.mc_class || g_cache.translatable_class || g_cache.main_hand)) {
            blockUnload();
            return;
        }
        CacheData retired;
        {
            std::lock_guard leaf(s_cache_mx);
            retired = static_cast<CacheData &>(g_cache);
            static_cast<CacheData &>(g_cache) = {};
            g_cache.ready.store(false, std::memory_order_release);
            g_cache.tried.store(false, std::memory_order_release);
        }
        release_cache(scope.get(), retired);
    }

    void onInit() override {
        silentaim::ActionCallbacks callbacks;
        callbacks.on_tick = [this](const silentaim::ActionContext &context) {
            // Only the input anchor precedes movement. The fallback callback
            // runs after movement and cannot emit slot changes or item uses.
            if (stage_ == Stage::Idle || context.position_final || context.frame_open ||
                silentaim::rotation_spoof_active())
                return;
            if (have_action_tick_ && last_action_tick_ == context.tick_id)
                return;
            have_action_tick_ = true;
            last_action_tick_ = context.tick_id;
            pump_control(context.tick_id);
        };
        silentaim::register_action_callbacks(this, callbacks);
        listen<events::GameLoopEvent>([this](events::GameLoopEvent &) {
            if (stage_ != Stage::Idle)
                silentaim::ensure_installed();
        });
    }

    bool deferredClick() const noexcept override { return true; }

  protected:
    void onEnable() override {
        // If a throw is currently active, do not wipe state mid-flight!
        if (stage_ != Stage::Idle && stage_ != Stage::Cleanup) {
            return;
        }
        if (stage_ == Stage::Cleanup) {
            return;
        }
        reset();
        double_pot_action_ = throws_.index() == 1;
        if (double_pot_action_)
            planned_throws_ = throwpot::maxThrows;
        stage_ = Stage::Selection;
    }

    void onDisable() override {
        disable_pending_ = false;
        if (selected_) {
            stage_ = Stage::Cleanup;
        } else {
            reset();
        }
    }

  private:
    EnumSetting potion_{this, "Potion", {"Healing", "Strength", "Speed"}, 0};
    ThrowPotThrowsSetting throws_{this};
    IntSetting cooldown_{this, "Pot Cooldown", 1, 1, 10};
    BoolSetting safe_mode_{this, "Safe Mode", true, "Prevents throwing extra potions when health is full."};

    enum class Stage { Idle, Selection, Launch, SlotChange, Restore, Cleanup };
    Stage stage_ = Stage::Idle;
    unsigned last_action_tick_ = 0;
    bool have_action_tick_ = false;
    int original_slot_ = -1;
    int potion_slot_ = -1;
    bool selected_ = false;
    bool disable_pending_ = false;
    unsigned thrown_ = 0;
    unsigned planned_throws_ = 1;
    ULONGLONG next_selection_at_ = 0;
    // Retained across activations to prevent repeated presses bypassing cooldown.
    ULONGLONG next_throw_at_ = 0;
    ULONGLONG restore_at_ = 0;
    unsigned last_throw_tick_ = 0;
    bool have_throw_tick_ = false;
    unsigned next_throw_tick_ = 0;
    std::uint32_t random_state_ = 0x8f3d9a71u;
    bool dynamic_plan_ready_ = false;
    bool double_pot_action_ = false;
    std::deque<int> dynamic_slots_;
    float predicted_health_ = 0.f;
    float maximum_health_ = 0.f;
    int selected_healing_points_ = 0;
    int next_slot_pending_ = -1;
    int next_healing_pending_ = 0;

    void reset() {
        stage_ = Stage::Idle;
        original_slot_ = -1;
        potion_slot_ = -1;
        selected_ = false;
        disable_pending_ = false;
        thrown_ = 0;
        planned_throws_ = 1;
        next_selection_at_ = 0;
        dynamic_plan_ready_ = false;
        double_pot_action_ = false;
        dynamic_slots_.clear();
        predicted_health_ = 0.f;
        maximum_health_ = 0.f;
        selected_healing_points_ = 0;
        next_slot_pending_ = -1;
        next_healing_pending_ = 0;
        have_throw_tick_ = false;
        last_throw_tick_ = 0;
    }

    void stop(const char *reason = nullptr) {
        if (reason && isEnabled())
            VLOG_INFO("throwpot", "not thrown: {}", reason);
        if (selected_) {
            stage_ = Stage::Cleanup;
            disable_pending_ = isEnabled();
            return;
        }
        stage_ = Stage::Idle;
        disable_pending_ = isEnabled();
    }

    void pump_control(unsigned tick_id) {
        // Run at the input anchor, before player movement.
        // Select before use and observe the configured interval before restoring.
        if (stage_ != Stage::Idle) {
            jvm::JniScope scope;
            if (scope) {
                auto *e = scope.get();
                if (e->PushLocalFrame(64) >= 0) {
                    build_cache(e);
                    if (g_cache.ready) {
                        auto mc = e->GetStaticObjectField(g_cache.mc_class, g_cache.mc_instance);
                        auto player = mc ? e->GetObjectField(mc, g_cache.mc_player) : nullptr;
                        if (!clear_exception(e) && mc && player) {
                            advance_action(e, mc, player, tick_id);
                        } else
                            stop("player unavailable");
                    } else
                        stop("runtime mappings unavailable");
                    e->PopLocalFrame(nullptr);
                }
            }
        }

        if (disable_pending_ && isEnabled()) {
            disable_pending_ = false;
            setEnabled(false);
        }
    }

    void finish_restore() {
        stage_ = Stage::Idle;
        original_slot_ = -1;
        potion_slot_ = -1;
        selected_ = false;
        thrown_ = 0;
        planned_throws_ = 1;
        next_selection_at_ = 0;
        dynamic_plan_ready_ = false;
        double_pot_action_ = false;
        dynamic_slots_.clear();
        predicted_health_ = 0.f;
        maximum_health_ = 0.f;
        selected_healing_points_ = 0;
        next_slot_pending_ = -1;
        next_healing_pending_ = 0;
        have_throw_tick_ = false;
        last_throw_tick_ = 0;
        if (isEnabled())
            disable_pending_ = true;
    }

    PotionKind selected_potion() const noexcept { return static_cast<PotionKind>(potion_.index()); }

    int next_delay_ms() noexcept {
        const bool randomized = !double_pot_action_;
        if (randomized)
            random_state_ = random_state_ * 1664525u + 1013904223u;
        return throwpot::throwDelayMs(cooldown_.value(), randomized, random_state_);
    }

    bool read_health(JNIEnv *env, jobject player, float &health, float &maximum) {
        health = env->CallFloatMethod(player, g_cache.living_health);
        if (clear_exception(env))
            return false;
        maximum = env->CallFloatMethod(player, g_cache.living_max_health);
        if (clear_exception(env))
            return false;
        return std::isfinite(health) && std::isfinite(maximum) && health > 0.f && maximum > 0.f;
    }

    bool find_potion_slot(JNIEnv *env, jobject inventory, PotionKind kind, int &found_slot, int &healing_points) {
        found_slot = -1;
        healing_points = 0;

        if (kind == PotionKind::Healing) {
            while (!dynamic_slots_.empty()) {
                const int slot = dynamic_slots_.front();
                int healing = 0;
                if (is_splash_potion(env, inventory, slot, kind, &healing)) {
                    found_slot = slot;
                    healing_points = healing;
                    return true;
                }
                // The hotbar changed after planning. Discard only the stale
                // entry; never substitute an unbudgeted potion.
                dynamic_slots_.pop_front();
            }
            return false;
        }

        // When possible, use the currently held potion without a slot switch.
        const jint held = env->GetIntField(inventory, g_cache.inventory_selected);
        if (!clear_exception(env) && held >= 0 && held < 9 && is_splash_potion(env, inventory, held, kind)) {
            found_slot = held;
            return true;
        }
        for (int slot = 0; slot < 9; ++slot) {
            int healing = 0;
            if (is_splash_potion(env, inventory, slot, kind, kind == PotionKind::Healing ? &healing : nullptr)) {
                found_slot = slot;
                healing_points = healing;
                return true;
            }
        }
        return false;
    }

    void build_dynamic_plan(JNIEnv *env, jobject inventory, float health, float maximum) {
        std::array<throwpot::Potion, 9> candidates{};
        std::size_t count = 0;
        for (int slot = 0; slot < 9; ++slot) {
            int healing = 0;
            if (is_splash_potion(env, inventory, slot, PotionKind::Healing, &healing))
                candidates[count++] = {slot, healing};
        }
        throwpot::Plan plan;
        const float plan_health = (!safe_mode_.value() && (health >= maximum || double_pot_action_)) ? 0.f : health;
        if (double_pot_action_) {
            plan = throwpot::doublePlan({candidates.data(), count}, plan_health, maximum);
        } else {
            random_state_ ^= random_state_ << 13;
            random_state_ ^= random_state_ >> 17;
            random_state_ ^= random_state_ << 5;
            plan = throwpot::randomPlan({candidates.data(), count}, plan_health, maximum, random_state_);
        }
        if (original_slot_ >= 0)
            plan = throwpot::preferHeld(plan, original_slot_);
        dynamic_slots_.clear();
        for (unsigned i = 0; i < plan.count; ++i)
            dynamic_slots_.push_back(plan.potions[i].slot);
        planned_throws_ = plan.count;
        dynamic_plan_ready_ = true;
    }
    void advance_action(JNIEnv *env, jobject mc, jobject player, unsigned tick_id) {
        if (stage_ == Stage::Idle || (!isEnabled() && stage_ != Stage::Cleanup))
            return;
        build_cache(env);
        if (!g_cache.ready) {
            stop("runtime mappings unavailable");
            return;
        }

        jobject inventory = env->GetObjectField(player, g_cache.player_inventory);
        jobject interaction = env->GetObjectField(mc, g_cache.mc_interaction);
        if (clear_exception(env) || !inventory || !interaction) {
            if (interaction)
                env->DeleteLocalRef(interaction);
            if (inventory)
                env->DeleteLocalRef(inventory);
            stop("inventory unavailable");
            return;
        }

        // Restore after the synchronous vanilla use returns, respecting manual input.
        if (stage_ == Stage::Cleanup || stage_ == Stage::Restore) {
            const bool tick_ready = have_throw_tick_ && (tick_id > last_throw_tick_);
            const bool time_ready = (GetTickCount64() + 15 >= restore_at_);
            if ((tick_ready || time_ready) && !right_mouse_down() && !player_is_using(env, player)) {

                const jint current = env->GetIntField(inventory, g_cache.inventory_selected);
                const bool read_ok = !clear_exception(env);
                if (read_ok && current == potion_slot_ && original_slot_ >= 0 && original_slot_ != potion_slot_) {
                    select_slot(env, interaction, inventory, original_slot_);
                }
                finish_restore();
            }
            env->DeleteLocalRef(interaction);
            env->DeleteLocalRef(inventory);
            return;
        }

        jobject screen = env->GetObjectField(mc, g_cache.mc_screen);
        const bool screen_open = !clear_exception(env) && screen != nullptr;
        if (screen)
            env->DeleteLocalRef(screen);
        const bool menu_open = screen_open || (vgui::isRunning() && vgui::isVisible());
        const bool input_busy = right_mouse_down() || player_is_using(env, player);
        const bool unfocused = !minecraft_has_focus();
        const jboolean riding = env->CallBooleanMethod(player, g_cache.entity_has_vehicle);
        const bool in_vehicle = !clear_exception(env) && riding == JNI_TRUE;

        if (menu_open || input_busy || unfocused || in_vehicle) {
            env->DeleteLocalRef(interaction);
            env->DeleteLocalRef(inventory);
            if (menu_open)
                stop("menu is open");
            else if (input_busy)
                stop("item use/right click is busy");
            else if (unfocused)
                stop("Minecraft is not focused");
            else
                stop("player is in a vehicle");
            return;
        }

        const ULONGLONG now_ms = GetTickCount64();
        const PotionKind kind = selected_potion();

        if (stage_ == Stage::Selection) {
            // Healing is evaluated on this key press only. Never leave a press
            // armed to fire later when health changes.
            if (kind == PotionKind::Healing) {
                float health = 0.f;
                float maximum = 0.f;
                if (!read_health(env, player, health, maximum)) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("health unavailable");
                    return;
                }
                maximum_health_ = maximum;
                predicted_health_ = std::max(predicted_health_, health);

                if (safe_mode_.value() && (health >= maximum - 0.01f || predicted_health_ >= maximum - 0.01f)) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    if (selected_)
                        stage_ = Stage::Restore;
                    else
                        stop("health is already full");
                    return;
                }
            }

            // Respect cooldown even when a new key press starts another action.
            const bool cooldown_tick_active = (next_throw_tick_ > 0 && tick_id < next_throw_tick_);
            const bool cooldown_time_active = (now_ms < next_throw_at_ && (now_ms + 15 < next_throw_at_));
            if (cooldown_tick_active && cooldown_time_active) {
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                return;
            }

            if (original_slot_ < 0) {
                original_slot_ = env->GetIntField(inventory, g_cache.inventory_selected);
                if (clear_exception(env) || original_slot_ < 0 || original_slot_ > 8) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("invalid selected hotbar slot");
                    return;
                }
            }

            if (kind == PotionKind::Healing && !dynamic_plan_ready_) {
                float health = 0.f;
                float maximum = 0.f;
                if (!read_health(env, player, health, maximum)) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("health unavailable");
                    return;
                }
                build_dynamic_plan(env, inventory, health, maximum);
                if (dynamic_slots_.empty()) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("no healing potion is available");
                    return;
                }
            }

            int found_slot = -1;
            int found_healing_points = 0;
            find_potion_slot(env, inventory, kind, found_slot, found_healing_points);
            if (found_slot < 0) {
                if (kind == PotionKind::Healing) {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("dynamic healing plan is no longer available");
                    return;
                }
                if (thrown_ > 0) {
                    VLOG_INFO("throwpot", "no more {} splash potions in the hotbar", potion_name(kind));
                    stage_ = Stage::Restore;
                } else {
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    stop("selected splash potion is not in the hotbar");
                    return;
                }
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                return;
            }
            potion_slot_ = found_slot;
            selected_healing_points_ = found_healing_points;

            if (!select_slot(env, interaction, inventory, potion_slot_)) {

                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                stop("could not select the potion slot");
                return;
            }

            selected_ = true;
            // The vanilla item-only use synchronizes the selected slot before
            // sending USE_ITEM. No additional selection tick is needed here.
            next_selection_at_ = now_ms;
            stage_ = Stage::Launch;
        } else if (stage_ == Stage::SlotChange) {
            const unsigned cooldown_ticks = static_cast<unsigned>((std::max)(1, cooldown_.value()));
            const bool tick_ready = have_throw_tick_ && (tick_id > last_throw_tick_) &&
                                    (tick_id - last_throw_tick_ >= cooldown_ticks);
            const bool time_ready = (now_ms + 15 >= next_selection_at_) && (now_ms + 15 >= next_throw_at_);
            if (!tick_ready && !time_ready) {
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                return;
            }

            int target_slot = next_slot_pending_;
            int target_healing = next_healing_pending_;

            if (target_slot < 0 || !is_splash_potion(env, inventory, target_slot, kind)) {
                int found_slot = -1;
                int found_healing = 0;
                find_potion_slot(env, inventory, kind, found_slot, found_healing);
                if (found_slot < 0) {
                    stage_ = Stage::Restore;
                    env->DeleteLocalRef(interaction);
                    env->DeleteLocalRef(inventory);
                    return;
                }
                target_slot = found_slot;
                target_healing = found_healing;
            }

            potion_slot_ = target_slot;
            selected_healing_points_ = target_healing;

            if (!select_slot(env, interaction, inventory, potion_slot_)) {

                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                stop("could not select the next potion slot");
                return;
            }

            selected_ = true;
            next_slot_pending_ = -1;
            next_healing_pending_ = 0;
            next_selection_at_ = now_ms;
            stage_ = Stage::Launch;
        }
        const unsigned cooldown_ticks = static_cast<unsigned>((std::max)(1, cooldown_.value()));
        const bool launch_tick_ready = !have_throw_tick_ || (tick_id > last_throw_tick_ &&
                                                             tick_id - last_throw_tick_ >= cooldown_ticks);
        const bool launch_time_ready = (now_ms + 15 >= (std::max)(next_selection_at_, next_throw_at_));
        if (stage_ == Stage::Launch && (launch_tick_ready || launch_time_ready)) {
            const jint current = env->GetIntField(inventory, g_cache.inventory_selected);
            if (clear_exception(env) || current != potion_slot_) {
                selected_ = false;
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                stop("hotbar slot changed before launch");
                return;
            }
            int actual_healing = 0;
            if (!is_splash_potion(env, inventory, potion_slot_, kind, &actual_healing)) {
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                stop("potion was removed before launch");
                return;
            }

            bool within_budget = thrown_ < planned_throws_ && thrown_ < throwpot::maxThrows;
            if (kind == PotionKind::Healing) {
                float health = 0.f, maximum = 0.f;
                within_budget = within_budget && read_health(env, player, health, maximum);
                const float estimated = std::max(health, predicted_health_);
                if (safe_mode_.value()) {
                    within_budget = within_budget && throwpot::needsHealing(estimated, maximum);
                }
                if (double_pot_action_ && within_budget) {
                    // Rebase on healing received since planning, before adding
                    // this throw. Otherwise a late health update can waste pot #2.
                    predicted_health_ = estimated;
                    maximum_health_ = maximum;
                }
                selected_healing_points_ = actual_healing;
            }
            if (!within_budget) {
                stage_ = Stage::Restore;
                env->DeleteLocalRef(interaction);
                env->DeleteLocalRef(inventory);
                return;
            }

            // The item-only vanilla path supplies the correct sequence id and
            // packet rotation without first interacting with the block under
            // the crosshair (the source of Matrix's interact.vis warning).
            // Selection was synchronized before this synchronous use call.
            stage_ = Stage::Cleanup;
            const ULONGLONG use_at = GetTickCount64();
            restore_at_ = use_at + throwpot::slotPauseMs;
            next_throw_at_ = use_at + static_cast<ULONGLONG>(next_delay_ms());
            have_throw_tick_ = true;
            last_throw_tick_ = tick_id;
            next_throw_tick_ = tick_id + cooldown_ticks;
            jobject result =
                env->CallObjectMethod(interaction, g_cache.interaction_use_item, player, g_cache.main_hand);
            if (clear_exception(env) || !result) {
                stage_ = Stage::Cleanup;
                VLOG_WARN("throwpot", "Minecraft rejected the item-only use");
            } else {
                const jboolean accepted = env->CallBooleanMethod(result, g_cache.result_is_accepted);
                if (!clear_exception(env) && accepted == JNI_TRUE) {
                    env->CallVoidMethod(player, g_cache.living_swing_hand, g_cache.main_hand);
                    clear_exception(env);
                    ++thrown_;

                    if (kind == PotionKind::Healing && maximum_health_ > 0.f) {
                        predicted_health_ =
                            std::min(maximum_health_,
                                     predicted_health_ + static_cast<float>(std::max(0, selected_healing_points_)));
                    }

                    if (kind == PotionKind::Healing && !dynamic_slots_.empty() &&
                        dynamic_slots_.front() == potion_slot_) {
                        dynamic_slots_.pop_front();
                    }

                    const bool action_done =
                        thrown_ >= planned_throws_ || thrown_ >= throwpot::maxThrows ||
                        (kind == PotionKind::Healing &&
                         (dynamic_slots_.empty() || (safe_mode_.value() && double_pot_action_ && predicted_health_ >= maximum_health_)));
                    if (action_done) {
                        stage_ = Stage::Restore;
                    } else {
                        // Wait before changing slots after item use.
                        int next_slot = -1;
                        int next_healing_points = 0;
                        if (!find_potion_slot(env, inventory, kind, next_slot, next_healing_points)) {
                            stage_ = Stage::Restore;
                        } else {
                            next_slot_pending_ = next_slot;
                            next_healing_pending_ = next_healing_points;
                            next_selection_at_ = restore_at_;
                            stage_ = Stage::SlotChange;
                        }
                    }

                    VLOG_INFO("throwpot", "threw {} potion {} from hotbar slot {}", potion_name(kind), thrown_,
                              potion_slot_ + 1);
                } else {
                    stage_ = Stage::Cleanup;
                    VLOG_WARN("throwpot", "item-only use returned pass");
                }
            }
            if (result)
                env->DeleteLocalRef(result);
        }

        env->DeleteLocalRef(interaction);
        env->DeleteLocalRef(inventory);
    }
};

VANTA_REGISTER_MODULE(ThrowPot);

} // namespace vanta::modules
