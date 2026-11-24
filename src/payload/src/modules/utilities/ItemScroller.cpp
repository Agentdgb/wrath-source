// ItemScroller -- shift-click and sweep across container slots to quick-move
// their stacks. Actions are queued, revalidated immediately before each click,
// and spaced by a random millisecond delay. The first slot gets a short vanilla
// grace period and is only sent by us if the physical click did not move it.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/setting/RangeSetting.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <sdk/mappings/mappings.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>

namespace vanta::modules {

namespace {

constexpr float VANILLA_FIRST_CLICK_GRACE_MS = 50.f;

struct Cache {
    bool ready = false;
    bool tried = false;

    jclass mc_class             = nullptr;
    jclass handled_screen_class = nullptr;
    jclass slot_class           = nullptr;
    jclass handler_class        = nullptr;
    jclass action_class         = nullptr;

    jfieldID mc_instance    = nullptr;
    jfieldID mc_screen      = nullptr;
    jfieldID mc_player      = nullptr;
    jfieldID mc_interaction = nullptr;

    jfieldID screen_hovered = nullptr;
    jfieldID screen_handler = nullptr;
    jfieldID slot_id        = nullptr;
    jfieldID handler_sync   = nullptr;

    jmethodID slot_has_stack = nullptr;
    jmethodID handler_slot   = nullptr;
    jmethodID click_slot     = nullptr;

    jobject quick_move = nullptr;
};

Cache g_cache;

void clear_exception(JNIEnv* env)
{
    if (env->ExceptionCheck()) env->ExceptionClear();
}

jclass find_class(JNIEnv* env, const char* name)
{
    jclass local = vanta::jvm::findClassAnyLoader(env, name);
    if (!local) {
        clear_exception(env);
        return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

void build_cache(JNIEnv* env)
{
    if (g_cache.tried) return;
    g_cache.tried = true;

    using namespace sdk::mappings;

    g_cache.mc_class             = find_class(env, minecraftclass_sig);
    g_cache.handled_screen_class = find_class(env, handled_screen_class_sig);
    g_cache.slot_class           = find_class(env, slot_class_sig);
    g_cache.handler_class        = find_class(env, screen_handler_class_sig);
    g_cache.action_class         = find_class(env, slot_action_type_class_sig);

    if (g_cache.mc_class) {
        g_cache.mc_instance = env->GetStaticFieldID(
            g_cache.mc_class, minecraftclient_name, minecraftclient_sig);
        clear_exception(env);
        g_cache.mc_screen = env->GetFieldID(
            g_cache.mc_class, minecraft_current_screen_name, minecraft_current_screen_sig);
        clear_exception(env);
        g_cache.mc_player = env->GetFieldID(
            g_cache.mc_class, player_name, player_sig);
        clear_exception(env);
        g_cache.mc_interaction = env->GetFieldID(
            g_cache.mc_class, interaction_manager_name, interaction_manager_sig);
        clear_exception(env);
    }

    if (g_cache.handled_screen_class) {
        g_cache.screen_hovered = env->GetFieldID(
            g_cache.handled_screen_class,
            handled_screen_focused_slot_name,
            handled_screen_focused_slot_sig);
        clear_exception(env);
        g_cache.screen_handler = env->GetFieldID(
            g_cache.handled_screen_class,
            handled_screen_handler_name,
            handled_screen_handler_sig);
        clear_exception(env);
    }

    if (g_cache.slot_class) {
        g_cache.slot_id = env->GetFieldID(
            g_cache.slot_class, slot_id_name, slot_id_sig);
        clear_exception(env);
        g_cache.slot_has_stack = env->GetMethodID(
            g_cache.slot_class, slot_has_stack_name, slot_has_stack_sig);
        clear_exception(env);
    }

    if (g_cache.handler_class) {
        g_cache.handler_sync = env->GetFieldID(
            g_cache.handler_class, screen_handler_sync_id_name, screen_handler_sync_id_sig);
        clear_exception(env);
        g_cache.handler_slot = env->GetMethodID(
            g_cache.handler_class, screen_handler_get_slot_name, screen_handler_get_slot_sig);
        clear_exception(env);
    }

    jclass interaction_class = find_class(env, interaction_manager_class_sig);
    if (interaction_class) {
        g_cache.click_slot = env->GetMethodID(
            interaction_class, click_slot_name, click_slot_sig);
        clear_exception(env);
        env->DeleteGlobalRef(interaction_class);
    }

    if (g_cache.action_class) {
        jfieldID quick_move_field = env->GetStaticFieldID(
            g_cache.action_class,
            slot_action_quick_move_name,
            slot_action_quick_move_sig);
        clear_exception(env);
        if (quick_move_field) {
            jobject local = env->GetStaticObjectField(g_cache.action_class, quick_move_field);
            clear_exception(env);
            if (local) {
                g_cache.quick_move = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
    }

    g_cache.ready = g_cache.mc_class && g_cache.handled_screen_class
        && g_cache.mc_instance && g_cache.mc_screen && g_cache.mc_player
        && g_cache.mc_interaction && g_cache.screen_hovered
        && g_cache.screen_handler && g_cache.slot_id && g_cache.slot_has_stack
        && g_cache.handler_sync && g_cache.handler_slot && g_cache.click_slot
        && g_cache.quick_move;

    VLOG_INFO("itemscroller",
              "cache: screen={} hovered={} handler={} slot={} click={} quick={} -> {}",
              g_cache.handled_screen_class ? 1 : 0,
              g_cache.screen_hovered ? 1 : 0,
              g_cache.screen_handler ? 1 : 0,
              g_cache.slot_id ? 1 : 0,
              g_cache.click_slot ? 1 : 0,
              g_cache.quick_move ? 1 : 0,
              g_cache.ready ? "ready" : "unusable");
}

bool game_is_foreground()
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

bool gesture_keys_down()
{
    const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0
        || (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    return left && shift;
}

int random_delay_ms(int minimum, int maximum)
{
    if (maximum < minimum) std::swap(minimum, maximum);
    if (maximum <= minimum) return minimum;
    static std::uint32_t state = 0xa341316cu;
    state = state * 1664525u + 1013904223u;
    return minimum + static_cast<int>(state
        % static_cast<std::uint32_t>(maximum - minimum + 1));
}

} // namespace

class ItemScroller final : public Module {
public:
    ItemScroller()
        : Module("ItemScroller",
                 "Shift-click and sweep over slots with a safe queued delay",
                 Category::Utilities)
    {
        registerSetting(&delay_);
        delay_.setDescription("Random delay between inventory moves.");
    }

    void onInit() override
    {
        listen<events::GameLoopEvent>(
            [this](events::GameLoopEvent& event) { pump(event.deltaMs); });
    }

    void onDisable() override { reset_gesture(); }

    std::string displayInfo() const override
    {
        if (delay_.min() == delay_.max())
            return std::to_string(delay_.min()) + "ms";
        return std::to_string(delay_.min()) + "-" + std::to_string(delay_.max()) + "ms";
    }

private:
    // Vape's inventory actions use a random 80-115 ms window by default. A
    // millisecond range is also clearer than exposing client ticks to users.
    // New key name intentionally avoids loading legacy "Delay" values that
    // were stored in ticks (for example 2-3) as if they were milliseconds.
    RangeSetting delay_{this, "Move Delay", 80, 115, 0, 500};

    bool gesture_active_ = false;
    int  active_sync_id_ = -1;
    int  last_hovered_   = -1;
    float cooldown_ms_   = 0.f;
    std::deque<int> queued_slots_;
    std::unordered_set<int> visited_slots_;

    void reset_gesture()
    {
        gesture_active_ = false;
        active_sync_id_ = -1;
        last_hovered_ = -1;
        cooldown_ms_ = 0.f;
        queued_slots_.clear();
        visited_slots_.clear();
    }

    void begin_gesture(int sync_id, int hovered_slot, bool hovered_has_stack)
    {
        reset_gesture();
        gesture_active_ = true;
        active_sync_id_ = sync_id;
        last_hovered_ = hovered_slot;

        if (hovered_slot >= 0) {
            visited_slots_.insert(hovered_slot);
            if (hovered_has_stack) queued_slots_.push_back(hovered_slot);
        }

        // Vanilla normally moves this first slot immediately. Waiting one
        // tick lets that happen; the pre-send stack check below then discards
        // it. If another client/input path swallowed the physical click, the
        // queued fallback makes the gesture work instead of skipping slot one.
        cooldown_ms_ = VANILLA_FIRST_CLICK_GRACE_MS;
    }

    void pump(float delta_ms)
    {
        if (!isEnabled() || ::vanta::vgui::bridge::isOverlayVisible()
            || !game_is_foreground() || !gesture_keys_down()) {
            reset_gesture();
            return;
        }

        vanta::jvm::JniScope scope;
        JNIEnv* env = scope.get();
        if (!env) return;

        build_cache(env);
        if (!g_cache.ready) return;

        jobject mc = env->GetStaticObjectField(g_cache.mc_class, g_cache.mc_instance);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            reset_gesture();
            return;
        }
        if (!mc) {
            reset_gesture();
            return;
        }

        jobject screen = env->GetObjectField(mc, g_cache.mc_screen);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            screen = nullptr;
        }
        if (!screen || env->IsInstanceOf(screen, g_cache.handled_screen_class) != JNI_TRUE) {
            if (screen) env->DeleteLocalRef(screen);
            env->DeleteLocalRef(mc);
            reset_gesture();
            return;
        }

        jobject handler = env->GetObjectField(screen, g_cache.screen_handler);
        clear_exception(env);
        if (!handler) {
            env->DeleteLocalRef(screen);
            env->DeleteLocalRef(mc);
            reset_gesture();
            return;
        }

        const int sync_id = env->GetIntField(handler, g_cache.handler_sync);
        clear_exception(env);

        int hovered_id = -1;
        bool hovered_has_stack = false;
        jobject hovered = env->GetObjectField(screen, g_cache.screen_hovered);
        clear_exception(env);
        if (hovered) {
            hovered_id = env->GetIntField(hovered, g_cache.slot_id);
            clear_exception(env);
            hovered_has_stack = env->CallBooleanMethod(hovered, g_cache.slot_has_stack) == JNI_TRUE;
            clear_exception(env);
            env->DeleteLocalRef(hovered);
        }

        if (!gesture_active_ || active_sync_id_ != sync_id) {
            begin_gesture(sync_id, hovered_id, hovered_has_stack);
            env->DeleteLocalRef(handler);
            env->DeleteLocalRef(screen);
            env->DeleteLocalRef(mc);
            return;
        }

        if (hovered_id != last_hovered_) {
            last_hovered_ = hovered_id;
            if (hovered_id >= 0 && hovered_has_stack
                && visited_slots_.insert(hovered_id).second) {
                queued_slots_.push_back(hovered_id);
            }
        }

        const float elapsed = (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.f;
        cooldown_ms_ = std::max(0.f, cooldown_ms_ - elapsed);

        if (cooldown_ms_ <= 0.f && !queued_slots_.empty()) {
            jobject player = env->GetObjectField(mc, g_cache.mc_player);
            clear_exception(env);
            jobject interaction = env->GetObjectField(mc, g_cache.mc_interaction);
            clear_exception(env);

            bool moved = false;
            while (player && interaction && !queued_slots_.empty() && !moved) {
                const int slot_id = queued_slots_.front();
                queued_slots_.pop_front();

                // Re-read the slot just before sending the click. Earlier
                // quick-moves can rearrange a container while slots wait in
                // the delay queue; empty entries must not become stray clicks.
                jobject slot = env->CallObjectMethod(handler, g_cache.handler_slot, slot_id);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    slot = nullptr;
                }
                if (!slot) continue;

                const bool has_stack =
                    env->CallBooleanMethod(slot, g_cache.slot_has_stack) == JNI_TRUE;
                clear_exception(env);
                env->DeleteLocalRef(slot);
                if (!has_stack) continue;

                env->CallVoidMethod(interaction, g_cache.click_slot,
                                    static_cast<jint>(sync_id),
                                    static_cast<jint>(slot_id),
                                    static_cast<jint>(0),
                                    g_cache.quick_move,
                                    player);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    VLOG_WARN("itemscroller", "quick-move failed for slot {}", slot_id);
                } else {
                    moved = true;
                }
            }

            if (moved) {
                cooldown_ms_ = static_cast<float>(
                    random_delay_ms(delay_.min(), delay_.max()));
            }

            if (interaction) env->DeleteLocalRef(interaction);
            if (player) env->DeleteLocalRef(player);
        }

        env->DeleteLocalRef(handler);
        env->DeleteLocalRef(screen);
        env->DeleteLocalRef(mc);
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::ItemScroller)
