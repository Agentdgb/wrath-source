#include "silent_totem_plan.h"
// autototem_tick.cpp — the AutoTotem state machine, driven by the render pump.
//
// No JNIHook anywhere. GameLoopEvent is pumped from the wglSwapBuffers detour,
// and in MC 1.21 that thread IS the thread that runs the game loop, so the JNI
// work below already lands on the game thread. Detouring MinecraftClient.tick
// to get "inside" the tick was tried and hard-froze the client: JNIHook runs
// the original through a renamed class copy whose statics are never
// initialised, and tick() is exactly the method that reaches for
// MinecraftClient.getInstance().

#include "autototem_tick.h"
#include "autototem.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <sdk/mappings/mappings.hpp>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace dt::modules::autototem_tick {

namespace tb = ::dt::modules::autototem;

static constexpr int TICK_MS       = 50;
static constexpr int RESEND_TICKS  = 20;   // 1 s
static constexpr int PLAYER_SYNC_ID = 0;   // your own screen handler is always 0
static constexpr int OFFHAND_BUTTON = 40;  // the off-hand hotkey button
static constexpr int INV_FIRST_SLOT = 9;   // main inventory, PlayerInventory index
static constexpr int INV_LAST_SLOT  = 35;

// ── state ────────────────────────────────────────────────────────────────────
static int  g_reaction_target_ms = 0;
static int  g_reaction_accum_ms  = 0;
static int  g_equip_clock        = 0;
static int  g_equip_target_ticks = 1;
static int  g_resend_clock       = 0;
static int  g_drain_checks       = 0;
static int  g_prev_slot          = -1;
static int  g_chosen_slot        = -1;
static bool g_sent               = false;
static bool g_active             = false;

// Anti-collision spacing between consecutive cycles.
static int  g_extra_defer_ticks  = 0;
static int  g_last_latency_ms    = -1;

// Inventory mode.
static bool g_we_opened_screen   = false;
static int  g_close_clock        = 0;
static int  g_close_target_ticks = 3;
// ── the input filter ─────────────────────────────────────────────────────────
//
// Raising it swallows the player's mouse buttons and key-downs at the window,
// before the game ever sees them. Two distinct jobs:
//
//   1. While a module-opened screen is up, so the player cannot fumble their
//      own inventory mid-swap.
//   2. While movement is being suppressed, because this pump runs inside the
//      same call where glfwPollEvents drains input — a Windows key-REPEAT
//      lands right after our write and re-presses the key we just zeroed.
//      Filtering the repeats is what makes the zero stick, and without it
//      Silent mode never actually stopped the player.
//
// Held as a countdown that each reason re-extends, so it can never outlive the
// reason for it. Nothing writes `true` back to a key — see hold_movement_keys.
//
// The level matters as much as the duration. Job 1 ends the moment the screen
// goes down, but the mouse must stay filtered a little longer for AutoTotemF —
// so the close DOWNGRADES 2 -> 1 rather than dropping the filter outright.
// Keeping level 2 through that tail is what made the player unable to walk for
// 150 ms after every re-totem: vanilla's own way of resuming a held key is the
// Windows key-REPEAT, and level 2 was eating exactly that.
static int g_lock_level      = 0;
static int g_lock_hold_ticks = 0;

// How long the filter stays up after the module's container interaction ends.
// The player's next click must not land adjacent to it: vanilla cannot place a
// block or swing in the same breath as a container action, and the server
// reads that pair as an invalid interaction.
static constexpr int POST_ACTION_LOCK_TICKS = 3;   // 150 ms
static constexpr int MAX_LOCK_TICKS         = 40;  // 2 s hard ceiling

// Filter levels, mirroring bridge::setInputFilter.
static constexpr int LOCK_OFF   = 0;
static constexpr int LOCK_MOUSE = 1;   // mouse buttons; keyboard still live
static constexpr int LOCK_FULL  = 2;   // mouse buttons + key-downs

// Stand-still gate for the click.
static int g_idle_ticks = 0;
static int g_hold_ticks = 0;
static constexpr int REQUIRED_IDLE_TICKS = 2;
static constexpr int MAX_HOLD_TICKS      = 20;   // 1 s

// Throttled trace, so a 20 Hz loop stays readable in the log.
#define AT_TRACE(tag, fmt_str, ...)                                            \
    do {                                                                       \
        static unsigned long long _next = 0;                                   \
        const unsigned long long _now = GetTickCount64();                      \
        if (_now >= _next) {                                                   \
            _next = _now + 500;                                                \
            VLOG_INFO("autototem", "[" tag "] " fmt_str, ##__VA_ARGS__);       \
        }                                                                      \
    } while (0)

// Survives reset() on purpose: it is a decision about the NEXT cycle.
static int g_reopen_backoff_ticks = 0;
static constexpr int REOPEN_BACKOFF_TICKS = 60;   // 3 s

// Silent mode.
static bool g_silent_session    = false;
static int  g_silent_tail_ticks = 0;   // also outlives reset()

static std::mt19937& rng()
{
    static std::mt19937 g(std::random_device{}());
    return g;
}

static void apply_lock_level(int level)
{
    if (level == g_lock_level) return;
    g_lock_level = level;
    ::dt::vgui::bridge::setInputFilter(level);
}

// Raise the filter, or extend it. Never shortens a hold and never downgrades a
// level: every reason that still applies on this pump keeps what it asked for.
static void lock_input_for(int ticks, int level)
{
    if (ticks > MAX_LOCK_TICKS) ticks = MAX_LOCK_TICKS;
    if (ticks > g_lock_hold_ticks) g_lock_hold_ticks = ticks;
    if (level > g_lock_level) apply_lock_level(level);
}

// Replace the hold outright, downgrades included. For the one point where a
// reason genuinely ENDS rather than lapses: the screen is down, so the keyboard
// goes straight back to the player while the mouse stays filtered a beat longer.
static void relock_input(int ticks, int level)
{
    if (ticks > MAX_LOCK_TICKS) ticks = MAX_LOCK_TICKS;
    g_lock_hold_ticks = ticks;
    apply_lock_level(level);
}

static void unlock_input()
{
    g_lock_hold_ticks = 0;
    apply_lock_level(LOCK_OFF);
}

void reset()
{
    g_reaction_target_ms = 0;
    g_reaction_accum_ms  = 0;
    g_equip_clock        = 0;
    g_equip_target_ticks = 1;
    g_resend_clock       = 0;
    g_drain_checks       = 0;
    g_prev_slot          = -1;
    g_chosen_slot        = -1;
    g_sent               = false;
    g_active             = false;
    g_extra_defer_ticks  = 0;
    g_we_opened_screen   = false;
    g_close_clock        = 0;
    g_idle_ticks         = 0;
    g_hold_ticks         = 0;

    // The filter is NOT dropped here. reset() runs the moment the cycle ends,
    // and the whole point of the post-action window is to outlive that: the
    // player's next click has to stay off the tick where the container
    // interaction landed. The pump's countdown owns the release.
}

// ── RAII local ref ───────────────────────────────────────────────────────────
namespace {
class LocalRef
{
public:
    LocalRef(JNIEnv* env, jobject ref) noexcept : env_(env), ref_(ref) {}
    ~LocalRef() { if (env_ && ref_) env_->DeleteLocalRef(ref_); }
    LocalRef(const LocalRef&)            = delete;
    LocalRef& operator=(const LocalRef&) = delete;

    jobject get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }

private:
    JNIEnv* env_;
    jobject ref_;
};
} // namespace

// ── reaction delay ───────────────────────────────────────────────────────────
//
// Flat uniform across the configured range, plus continuous jitter, plus a
// randomised equip width. Flat rather than bell-shaped or bimodal: a mixture
// leaves a sparse slow tail that drags the mean up so nearly every sample sits
// below it, which is the "low outliers" signature. A flat draw has neither a
// tail nor a modal peak.
static void sample_reaction_delay()
{
    int lo = tb::settings::reaction_min_ms.load(std::memory_order_relaxed);
    int hi = tb::settings::reaction_max_ms.load(std::memory_order_relaxed);
    if (lo < 0) lo = 0;
    if (hi < lo) hi = lo;

    double draw;
    if (tb::settings::randomization.load(std::memory_order_relaxed)) {
        std::uniform_real_distribution<double> flat(static_cast<double>(lo),
                                                    static_cast<double>(hi));
        draw = flat(rng());
    } else {
        draw = 0.5 * (lo + hi);
    }

    // Continuous jitter, added BEFORE the pump requantises to 50 ms. Without
    // it the effective distribution collapses onto a handful of discrete
    // buckets and consecutive cycles start landing in the same one.
    std::uniform_real_distribution<double> jitter(-50.0, 50.0);
    draw += jitter(rng());
    if (draw < 0.0) draw = 0.0;

    g_reaction_target_ms = static_cast<int>(draw + 0.5);
    g_reaction_accum_ms  = 0;

    // Hotbar needs a real minimum: the wait is what lets the slot-change
    // packet be processed BEFORE the swap follows it. Send them together and
    // the server swaps whatever it still thinks is held — the "sword in the
    // off-hand" bug. Inventory/Silent carry the slot in the click itself, so
    // their wait is pure pacing and can be shorter.
    const bool hotbar =
        static_cast<tb::Mode>(tb::settings::mode.load(std::memory_order_relaxed))
            == tb::Mode::Hotbar;

    if (hotbar) {
        const int equip_min = std::max(1, tb::settings::equip_delay.load(std::memory_order_relaxed));
        std::uniform_int_distribution<int> extra(0, 2);
        g_equip_target_ticks = equip_min + extra(rng());
    } else {
        std::uniform_int_distribution<int> extra(1, 2);
        g_equip_target_ticks = extra(rng());
    }
}

// ── item helpers ─────────────────────────────────────────────────────────────

static bool stack_is_totem(JNIEnv* env, const tb::jni_cache& c, jobject stack)
{
    if (!stack || !c.itemstack_is_empty_mid || !c.itemstack_get_item_mid
        || !c.item_get_translation_key_mid) return false;

    if (env->CallBooleanMethod(stack, c.itemstack_is_empty_mid) == JNI_TRUE) return false;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    LocalRef item(env, env->CallObjectMethod(stack, c.itemstack_get_item_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!item) return false;

    LocalRef key(env, env->CallObjectMethod(item.get(), c.item_get_translation_key_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!key) return false;

    const char* utf = env->GetStringUTFChars(static_cast<jstring>(key.get()), nullptr);
    if (!utf) return false;
    const bool hit = std::strstr(utf, "totem_of_undying") != nullptr;
    env->ReleaseStringUTFChars(static_cast<jstring>(key.get()), utf);
    return hit;
}

static jobject inventory_of(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.player_inventory_fid) return nullptr;
    jobject inv = env->GetObjectField(player, c.player_inventory_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return inv;
}

// Reads the off-hand through PlayerInventory.getStack(40).
//
// The combined list is main(36) + armor(4) + offHand(1), so index 40 IS the
// off-hand — and getStack is the same call that already finds totems, i.e. a
// mapping proven live. getOffHandStack is kept only as a fallback: it resolves
// on this host but was not reporting the totem the module had just equipped,
// which left the module clicking in a loop forever.
static jobject offhand_stack(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (c.inventory_get_stack_mid) {
        LocalRef inv(env, inventory_of(env, c, player));
        if (inv) {
            jobject s = env->CallObjectMethod(inv.get(), c.inventory_get_stack_mid, 40);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (s) return s;
        }
    }
    if (!c.get_off_hand_stack_mid) return nullptr;
    jobject s = env->CallObjectMethod(player, c.get_off_hand_stack_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return s;
}

static bool offhand_has_totem(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    LocalRef s(env, offhand_stack(env, c, player));
    return s && stack_is_totem(env, c, s.get());
}

static bool mainhand_has_totem(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.get_main_hand_stack_mid) return false;
    LocalRef s(env, env->CallObjectMethod(player, c.get_main_hand_stack_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return s && stack_is_totem(env, c, s.get());
}

static int read_selected_slot(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.inventory_selected_slot_fid) return -1;
    LocalRef inv(env, inventory_of(env, c, player));
    if (!inv) return -1;
    jint slot = env->GetIntField(inv.get(), c.inventory_selected_slot_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    return (slot >= 0 && slot <= 8) ? static_cast<int>(slot) : -1;
}

// Sets the held hotbar slot the way vanilla does: write selectedSlot and let
// ClientPlayerInteractionManager.tick() notice and emit the packet from its own
// call site. Calling syncSelectedSlot ourselves from the render pump is what
// put the packet inside Grim's open Post window.
static bool set_selected_slot(JNIEnv* env, const tb::jni_cache& c,
    jobject mc, jobject player, int slot)
{
    if (slot < 0 || slot > 8 || !c.inventory_selected_slot_fid) return false;

    LocalRef inv(env, inventory_of(env, c, player));
    if (!inv) return false;

    if (env->GetIntField(inv.get(), c.inventory_selected_slot_fid) != slot) {
        env->SetIntField(inv.get(), c.inventory_selected_slot_fid, slot);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        // Only sync by hand when the vanilla keybind route is unavailable —
        // there an unsynced slot would make the swap grab the wrong item.
        if (!c.keybind_route_usable && c.mc_interaction_manager_fid && c.sync_selected_slot_mid) {
            LocalRef im(env, env->GetObjectField(mc, c.mc_interaction_manager_fid));
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (im) {
                env->CallVoidMethod(im.get(), c.sync_selected_slot_mid);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }
    return true;
}

// Is this PlayerInventory index still holding a totem?
//
// The chosen slot is latched for the whole cycle, and 1-2 ticks pass between
// choosing it and acting on it. In that gap the contents can move: the
// previous cycle's swap puts the old off-hand item somewhere, or the player
// shuffles their own inventory. Acting on a stale slot means swapping a
// NON-totem into the off-hand, which the server sees as a totem interaction
// that makes no sense — TotemGuard reports it as "invalid interaction".
//
// So the latch is revalidated every time it is used, and dropped if it no
// longer points at a totem.
static bool slot_still_holds_totem(JNIEnv* env, const tb::jni_cache& c,
    jobject player, int inv_index)
{
    if (inv_index < 0 || !c.inventory_get_stack_mid) return false;
    LocalRef inv(env, inventory_of(env, c, player));
    if (!inv) return false;

    LocalRef stack(env, env->CallObjectMethod(inv.get(), c.inventory_get_stack_mid, inv_index));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return stack && stack_is_totem(env, c, stack.get());
}

// Picks a hotbar slot holding a totem, at random among all of them when
// randomization is on. Latched by the caller for the whole cycle: re-rolling
// per tick made the hand hop between totems mid-cycle and the swap then fired
// on whichever roll happened to land last.
static int find_totem_in_hotbar(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.inventory_get_stack_mid) return -1;
    LocalRef inv(env, inventory_of(env, c, player));
    if (!inv) return -1;

    int slots[9];
    int count = 0;
    for (int slot = 0; slot <= 8; ++slot) {
        LocalRef stack(env, env->CallObjectMethod(inv.get(), c.inventory_get_stack_mid, slot));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (stack && stack_is_totem(env, c, stack.get())) slots[count++] = slot;
    }
    if (count == 0) return -1;

    int pick = 0;
    if (count > 1 && tb::settings::randomization.load(std::memory_order_relaxed)) {
        std::uniform_int_distribution<int> d(0, count - 1);
        pick = d(rng());
    }
    return slots[pick];
}

// Main inventory, PlayerInventory indices 9-35 — which map 1:1 onto the
// container slots the click uses.
static int find_totem_in_inventory(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.inventory_get_stack_mid) return -1;
    LocalRef inv(env, inventory_of(env, c, player));
    if (!inv) return -1;

    int slots[INV_LAST_SLOT - INV_FIRST_SLOT + 1];
    int count = 0;
    for (int slot = INV_FIRST_SLOT; slot <= INV_LAST_SLOT; ++slot) {
        LocalRef stack(env, env->CallObjectMethod(inv.get(), c.inventory_get_stack_mid, slot));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (stack && stack_is_totem(env, c, stack.get())) slots[count++] = slot;
    }
    if (count == 0) return -1;

    int pick = 0;
    if (count > 1 && tb::settings::randomization.load(std::memory_order_relaxed)) {
        std::uniform_int_distribution<int> d(0, count - 1);
        pick = d(rng());
    }
    return slots[pick];
}

// ── screens ──────────────────────────────────────────────────────────────────

static jobject current_screen(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.mc_current_screen_fid) return nullptr;
    jobject s = env->GetObjectField(mc, c.mc_current_screen_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return s;
}

static bool screen_is_open(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    LocalRef s(env, current_screen(env, c, mc));
    return s.get() != nullptr;
}

static bool inventory_screen_open(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.inventory_screen_class) return false;
    LocalRef s(env, current_screen(env, c, mc));
    if (!s) return false;
    return env->IsInstanceOf(s.get(), c.inventory_screen_class) == JNI_TRUE;
}

static bool open_inventory_screen(JNIEnv* env, const tb::jni_cache& c, jobject mc, jobject player)
{
    if (!c.inventory_screen_class || !c.inventory_screen_ctor || !c.mc_set_screen_mid)
        return false;

    LocalRef screen(env, env->NewObject(c.inventory_screen_class,
                                        c.inventory_screen_ctor, player));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!screen) return false;

    env->CallVoidMethod(mc, c.mc_set_screen_mid, screen.get());
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static bool send_close_screen_packet(JNIEnv* env, const tb::jni_cache& c, jobject player);

// Closes a module-opened screen, packet AND client state.
//
// setScreen(null) alone is NOT enough, and assuming it was is what kept
// Inventory mode flagging AutoTotemF while Silent mode stopped:
//
//   HandledScreen.close()   -> player.closeHandledScreen() -> SENDS the packet
//   HandledScreen.removed() -> handler.onClosed(player)    -> client cleanup only
//
// setScreen(null) runs removed(), not close(). So the screen vanished locally
// while the server never heard about it — and AutoTotemF is disarmed by
// InventoryCloseEvent, which only fires when the server receives that packet.
// The timer stayed armed and the player's next click failed the check.
//
// Order mirrors vanilla's own close path: packet first, then tear the screen
// down locally.
static void close_screen(JNIEnv* env, const tb::jni_cache& c, jobject mc, jobject player)
{
    if (player) send_close_screen_packet(env, c, player);

    if (!c.mc_set_screen_mid) return;
    env->CallVoidMethod(mc, c.mc_set_screen_mid, nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// ── the digging gate ─────────────────────────────────────────────────────────
// SWAP_ITEM_WITH_OFFHAND is itself a PlayerActionC2SPacket — the same family as
// START_DESTROY_BLOCK and friends. Slipping a swap between those is what Grim
// reports as "post player digging". Holding it while a break is live costs
// nothing: the totem equips the moment you stop mining.
static bool digging_active(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.mc_options_fid || !c.options_attack_key_fid || !c.key_binding_is_pressed_mid
        || !c.mc_crosshair_target_fid || !c.block_hit_result_class) return false;

    LocalRef options(env, env->GetObjectField(mc, c.mc_options_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!options) return false;

    LocalRef attack_key(env, env->GetObjectField(options.get(), c.options_attack_key_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!attack_key) return false;

    const jboolean pressed = env->CallBooleanMethod(attack_key.get(), c.key_binding_is_pressed_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (pressed != JNI_TRUE) return false;

    LocalRef hit(env, env->GetObjectField(mc, c.mc_crosshair_target_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!hit) return false;

    return env->IsInstanceOf(hit.get(), c.block_hit_result_class) == JNI_TRUE;
}

// ── keybind resolution (reflective, by translation key) ──────────────────────
static bool resolve_keybinds(JNIEnv* env, tb::jni_cache& c, jobject options)
{
    if (c.keybinds_resolved) return c.keybind_route_usable;
    c.keybinds_resolved = true;

    if (!options || !c.key_binding_class || !c.key_binding_get_translation_mid) return false;

    LocalRef opts_cls(env, env->GetObjectClass(options));
    LocalRef class_cls(env, env->FindClass("java/lang/Class"));
    LocalRef field_cls(env, env->FindClass("java/lang/reflect/Field"));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!opts_cls || !class_cls || !field_cls) return false;

    jmethodID get_fields = env->GetMethodID(static_cast<jclass>(class_cls.get()),
        "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID get_type   = env->GetMethodID(static_cast<jclass>(field_cls.get()),
        "getType", "()Ljava/lang/Class;");
    jmethodID set_access = env->GetMethodID(static_cast<jclass>(field_cls.get()),
        "setAccessible", "(Z)V");
    jmethodID field_get  = env->GetMethodID(static_cast<jclass>(field_cls.get()),
        "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!get_fields || !get_type || !set_access || !field_get) return false;

    LocalRef arr(env, env->CallObjectMethod(opts_cls.get(), get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!arr) return false;

    static const char* kMovement[6] = {
        "key.forward", "key.back", "key.left", "key.right", "key.jump", "key.sneak"
    };

    auto fields = static_cast<jobjectArray>(arr.get());
    const jsize n = env->GetArrayLength(fields);

    for (jsize i = 0; i < n; ++i) {
        LocalRef f(env, env->GetObjectArrayElement(fields, i));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!f) continue;

        LocalRef ftype(env, env->CallObjectMethod(f.get(), get_type));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!ftype) continue;
        if (env->IsSameObject(ftype.get(), c.key_binding_class) != JNI_TRUE) continue;

        env->CallVoidMethod(f.get(), set_access, JNI_TRUE);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

        LocalRef kb(env, env->CallObjectMethod(f.get(), field_get, options));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;

        LocalRef key(env, env->CallObjectMethod(kb.get(), c.key_binding_get_translation_mid));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!key) continue;

        const char* utf = env->GetStringUTFChars(static_cast<jstring>(key.get()), nullptr);
        if (!utf) continue;

        bool is_swap = (std::strcmp(utf, "key.swapOffhand") == 0)
                    || (std::strstr(utf, "swapOffhand") != nullptr)
                    || (std::strstr(utf, "swapHands")   != nullptr);
        int movement_slot = -1;
        for (int k = 0; k < 6; ++k)
            if (std::strcmp(utf, kMovement[k]) == 0) { movement_slot = k; break; }
        env->ReleaseStringUTFChars(static_cast<jstring>(key.get()), utf);

        if (!is_swap && movement_slot < 0) continue;

        jfieldID fid = env->FromReflectedField(f.get());
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!fid) continue;

        if (is_swap) {
            c.options_swap_hands_key_fid = fid;
            c.keybind_route_usable = (c.key_binding_times_pressed_fid != nullptr);
        } else if (!c.movement_key_fids[movement_slot]) {
            c.movement_key_fids[movement_slot] = fid;
            ++c.movement_key_count;
        }
    }

    VLOG_INFO("autototem", "keybinds: swap={} movement={}/6",
              c.keybind_route_usable ? "usable" : "NO", c.movement_key_count);
    return c.keybind_route_usable;
}

// ── the swap ─────────────────────────────────────────────────────────────────

// Queues a vanilla F press. handleKeybinds() drains it INSIDE the tick and
// emits PlayerActionC2SPacket from vanilla's own call site — which is the whole
// reason Hotbar mode makes no inventory noise and never meets the
// MultiActions family.
static bool request_vanilla_swap(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.keybind_route_usable || !c.mc_options_fid
        || !c.options_swap_hands_key_fid || !c.key_binding_times_pressed_fid) return false;

    LocalRef options(env, env->GetObjectField(mc, c.mc_options_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!options) return false;

    LocalRef kb(env, env->GetObjectField(options.get(), c.options_swap_hands_key_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!kb) return false;

    env->SetIntField(kb.get(), c.key_binding_times_pressed_fid, 1);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static bool keypress_drained(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.keybind_route_usable) return false;
    LocalRef options(env, env->GetObjectField(mc, c.mc_options_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!options) return false;
    LocalRef kb(env, env->GetObjectField(options.get(), c.options_swap_hands_key_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!kb) return false;
    const jint left = env->GetIntField(kb.get(), c.key_binding_times_pressed_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return left == 0;
}

// Direct PlayerActionC2SPacket, used only when the vanilla keybind route could
// not be resolved on this host.
static bool send_swap_packet(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.player_action_packet_class || !c.player_action_packet_ctor
        || !c.swap_offhand_action_ref || !c.block_pos_origin_ref
        || !c.direction_down_ref || !c.player_network_handler_fid
        || !c.send_packet_mid) return false;

    LocalRef nh(env, env->GetObjectField(player, c.player_network_handler_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!nh) return false;

    LocalRef pkt(env, env->NewObject(c.player_action_packet_class,
                                     c.player_action_packet_ctor,
                                     c.swap_offhand_action_ref,
                                     c.block_pos_origin_ref,
                                     c.direction_down_ref));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!pkt) return false;

    env->CallVoidMethod(nh.get(), c.send_packet_mid, pkt.get());
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static bool movement_is_idle(JNIEnv* env, const tb::jni_cache& c, jobject mc);

static bool click_swap_to_offhand(JNIEnv* env, const tb::jni_cache& c,
    jobject mc, jobject player, int container_slot, int swap_button = OFFHAND_BUTTON)
{
    if (!c.click_slot_mid || !c.slot_action_swap_ref || !c.mc_interaction_manager_fid)
        return false;

    // Record the state the click actually goes out in. If a MultiActions alert
    // ever lines up with `idle=0` here, the suppression did not take and the
    // problem is upstream of the timing — that is a fact, not a guess.
    VLOG_INFO("autototem", "click slot={} idle={} lock={}",
              container_slot, (int)movement_is_idle(env, c, mc), g_lock_level);

    LocalRef im(env, env->GetObjectField(mc, c.mc_interaction_manager_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!im) return false;

    env->CallVoidMethod(im.get(), c.click_slot_mid,
                        static_cast<jint>(PLAYER_SYNC_ID),
                        static_cast<jint>(container_slot),
                        static_cast<jint>(swap_button),
                        c.slot_action_swap_ref,
                        player);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static bool send_close_screen_packet(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!c.close_screen_packet_class || !c.close_screen_packet_ctor
        || !c.player_network_handler_fid || !c.send_packet_mid) return false;

    LocalRef nh(env, env->GetObjectField(player, c.player_network_handler_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!nh) return false;

    LocalRef pkt(env, env->NewObject(c.close_screen_packet_class,
                                     c.close_screen_packet_ctor,
                                     static_cast<jint>(PLAYER_SYNC_ID)));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!pkt) return false;

    env->CallVoidMethod(nh.get(), c.send_packet_mid, pkt.get());
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

// ── movement suppression (Silent mode only) ──────────────────────────────────
//
// This function only ever writes FALSE. There is deliberately no "restore to
// true" counterpart.
//
// Writing true would press all six bindings at once — forward, back, left,
// right, jump AND sneak — which is exactly the "it jumps, strafes and crouches
// on its own" behaviour a restore-by-writing-true produced. There is also no
// honest value to restore: we cannot know which keys the player is still
// holding, and guessing wrong walks their character off by itself.
//
// Releasing is simply ceasing to call this. The keys are left false, and if
// the player is still holding one, Windows key-repeat re-presses it within
// ~33 ms with the TRUE state. If they let go, it correctly stays released.
static void hold_movement_keys(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    if (!c.mc_options_fid || c.movement_key_count == 0) return;
    if (!c.key_binding_pressed_fid && !c.key_binding_set_pressed_mid) return;

    LocalRef options(env, env->GetObjectField(mc, c.mc_options_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;

    constexpr jboolean v = JNI_FALSE;
    for (int i = 0; i < 6; ++i) {
        if (!c.movement_key_fids[i]) continue;
        LocalRef kb(env, env->GetObjectField(options.get(), c.movement_key_fids[i]));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;

        // The raw field is what the movement code reads and needs no guessed
        // method name; setPressed is called after only for any bookkeeping.
        if (c.key_binding_pressed_fid) {
            env->SetBooleanField(kb.get(), c.key_binding_pressed_fid, v);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (c.key_binding_set_pressed_mid) {
            env->CallVoidMethod(kb.get(), c.key_binding_set_pressed_mid, v);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
}

// Reads a KeyBinding's pressed state, preferring the raw field over the
// guessed isPressed() mapping. Never calls wasPressed(): it shares the ()Z
// shape and has a side effect (it decrements timesPressed), so probing for a
// boolean getter by signature would quietly eat queued key presses.
static bool key_is_pressed(JNIEnv* env, const tb::jni_cache& c, jobject kb)
{
    if (c.key_binding_pressed_fid) {
        const jboolean v = env->GetBooleanField(kb, c.key_binding_pressed_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else return v == JNI_TRUE;
    }
    if (c.key_binding_is_pressed_mid) {
        const jboolean v = env->CallBooleanMethod(kb, c.key_binding_is_pressed_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else return v == JNI_TRUE;
    }
    return false;
}

// True when every movement binding currently reads as NOT pressed.
//
// This is a PRECONDITION for the click, not an assumption. Forcing the keys to
// false and clicking in the same breath only works if the force took — and it
// often does not, because a Windows key-repeat can land between the write and
// the tick that reads it. Reading the state back is the difference between
// "we asked the player to stop" and "the player has stopped".
static bool movement_is_idle(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    const bool can_read = c.key_binding_pressed_fid || c.key_binding_is_pressed_mid;
    if (!can_read || !c.mc_options_fid || c.movement_key_count == 0)
        return true;   // cannot tell — never deadlock the module on it

    LocalRef options(env, env->GetObjectField(mc, c.mc_options_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!options) return true;

    for (int i = 0; i < 6; ++i) {
        if (!c.movement_key_fids[i]) continue;
        LocalRef kb(env, env->GetObjectField(options.get(), c.movement_key_fids[i]));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;
        if (key_is_pressed(env, c, kb.get())) return false;
    }
    return true;
}

// ── MinecraftClient.getInstance() ────────────────────────────────────────────
static jobject resolve_minecraft(JNIEnv* env, const tb::jni_cache& c)
{
    if (!c.minecraft_class) return nullptr;
    jfieldID fid = env->GetStaticFieldID(c.minecraft_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!fid) return nullptr;
    jobject mc = env->GetStaticObjectField(c.minecraft_class, fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return mc;
}

// Shared anti-collision spacing: if this cycle's total wire latency would land
// in the same 50 ms bucket as the previous one and the pair mean is under
// 500 ms, defer a tick. Consecutive identical intervals are the one thing the
// stddev-based totem checks key on.
static bool defer_for_collision(int this_latency)
{
    if (g_last_latency_ms < 0 || g_extra_defer_ticks >= 2) return false;
    const int    delta = std::abs(this_latency - g_last_latency_ms);
    const double mean  = 0.5 * (this_latency + g_last_latency_ms);
    if (delta < 43 && mean < 500.0) {
        ++g_equip_target_ticks;
        ++g_extra_defer_ticks;
        return true;
    }
    return false;
}

// ── HOTBAR MODE ──────────────────────────────────────────────────────────────
static void step_hotbar(JNIEnv* env, const tb::jni_cache& c, jobject mc, LocalRef& player)
{
    if (offhand_has_totem(env, c, player.get())) {
        if (g_active) {
            if (tb::settings::return_original.load(std::memory_order_relaxed) && g_prev_slot != -1)
                set_selected_slot(env, c, mc, player.get(), g_prev_slot);
            reset();
        }
        return;
    }

    if (!g_active) {
        g_active    = true;
        g_prev_slot = read_selected_slot(env, c, player.get());
        sample_reaction_delay();
    }

    if (g_reaction_accum_ms < g_reaction_target_ms) {
        g_reaction_accum_ms += TICK_MS;
        return;
    }

    // Latch the slot once per cycle; re-rolling per tick made the hand hop.
    // Revalidate it every tick — a latch that has gone stale would select a
    // slot that no longer holds a totem.
    if (g_chosen_slot >= 0 && !slot_still_holds_totem(env, c, player.get(), g_chosen_slot))
        g_chosen_slot = -1;
    if (g_chosen_slot < 0) g_chosen_slot = find_totem_in_hotbar(env, c, player.get());
    if (g_chosen_slot < 0) return;
    if (!set_selected_slot(env, c, mc, player.get(), g_chosen_slot)) return;

    if (g_equip_clock < g_equip_target_ticks) { ++g_equip_clock; return; }
    if (digging_active(env, c, mc)) return;

    if (!g_sent) {
        // The main hand must ACTUALLY hold the totem. Selecting the slot only
        // requests the change; swapping before it lands puts whatever you were
        // carrying into the off-hand instead.
        if (!mainhand_has_totem(env, c, player.get())) return;

        const int this_latency = g_reaction_accum_ms + g_equip_target_ticks * TICK_MS;
        if (defer_for_collision(this_latency)) return;

        const bool queued = request_vanilla_swap(env, c, mc)
                         || send_swap_packet(env, c, player.get());
        if (queued) {
            g_sent            = true;
            g_resend_clock    = 0;
            g_drain_checks    = 0;
            g_last_latency_ms = this_latency;
        }
    } else {
        // Watchdog: if vanilla never drains the queued press, send directly
        // rather than sit here forever with the totem stuck in the main hand.
        if (c.keybind_route_usable && ++g_drain_checks >= 6) {
            g_drain_checks = 0;
            if (!keypress_drained(env, c, mc)) {
                VLOG_WARN("autototem", "vanilla keybind not draining — sending directly");
                send_swap_packet(env, c, player.get());
            }
        }
        if (++g_resend_clock > RESEND_TICKS) {
            g_sent         = false;
            g_resend_clock = 0;
            g_drain_checks = 0;
        }
    }
}

// ── INVENTORY MODE ───────────────────────────────────────────────────────────
static void step_inventory(JNIEnv* env, const tb::jni_cache& c, jobject mc, LocalRef& player)
{
    const auto open_mode = static_cast<tb::OpenMode>(
        tb::settings::open_mode.load(std::memory_order_relaxed));

    if (offhand_has_totem(env, c, player.get())) {
        if (g_active) {
            const bool should_close =
                g_we_opened_screen
                || (open_mode == tb::OpenMode::OnOpen
                    && tb::settings::auto_close.load(std::memory_order_relaxed));

            if (should_close && inventory_screen_open(env, c, mc)) {
                // Never close on the same tick as the click.
                if (++g_close_clock < g_close_target_ticks) return;
                close_screen(env, c, mc, player.get());
            }
            // Hold the filter a beat past the close, but only over the MOUSE.
            // Releasing it entirely lets the player's very next click land
            // adjacent to the container interaction, which is what flagged when
            // someone attacked or placed a block right after a re-totem.
            //
            // Downgrading rather than extending is the point. Opening a screen
            // makes vanilla call KeyBinding.unpressAll(), and the only thing
            // that presses those keys back is the Windows key-REPEAT — which a
            // level-2 filter eats. Holding full lock through this tail left the
            // player standing still for 150 ms after every re-totem. At level 1
            // the very next repeat resumes their walk, with no key synthesised
            // by us at all.
            relock_input(POST_ACTION_LOCK_TICKS, LOCK_MOUSE);
            reset();
        }
        return;
    }

    if (g_reopen_backoff_ticks > 0) { --g_reopen_backoff_ticks; return; }

    if (!g_active) {
        // Do not start a cycle we cannot finish: with no totem in the main
        // inventory the cycle would stay live forever, and in AutoOpen every
        // tick would reopen the screen the player just closed.
        if (find_totem_in_inventory(env, c, player.get()) < 0) return;

        g_active = true;
        sample_reaction_delay();
        g_we_opened_screen = false;
        g_close_clock      = 0;
        // 1-2 ticks, not 2-5. AutoTotemF stays armed from the totem click
        // until an InventoryCloseEvent, and any world interaction inside that
        // window fails the check — so the close has to land well before the
        // input filter drops and the player can click again. One tick of gap
        // is still enough that we never close on the click's own tick.
        std::uniform_int_distribution<int> d(1, 2);
        g_close_target_ticks = d(rng());
    }

    // The player closed a screen WE opened → they want out.
    if (g_we_opened_screen && !inventory_screen_open(env, c, mc)) {
        VLOG_INFO("autototem", "player closed the inventory — backing off");
        reset();
        g_reopen_backoff_ticks = REOPEN_BACKOFF_TICKS;
        return;
    }

    if (g_reaction_accum_ms < g_reaction_target_ms) {
        g_reaction_accum_ms += TICK_MS;
        return;
    }

    if (!inventory_screen_open(env, c, mc)) {
        if (open_mode == tb::OpenMode::OnOpen) return;      // purely reactive
        if (screen_is_open(env, c, mc)) return;             // never fight a chest
        if (!open_inventory_screen(env, c, mc, player.get())) return;
        g_we_opened_screen = true;
        // setScreen is synchronous, so fall through to the equip pacing
        // instead of burning a whole pump tick here.
    }

    // Freeze the player's item-moving input, but ONLY when WE opened the
    // screen. In OnOpen the inventory is theirs and they are using it; locking
    // there takes their session away, and if the cycle stalls they are stuck.
    if (g_we_opened_screen) lock_input_for(MAX_LOCK_TICKS, LOCK_FULL);

    // Stand-still gate — the same one Silent has always had, and the reason
    // Inventory kept producing a rare MultiActionsC.
    //
    // Opening a screen makes vanilla unpressAll(), so the keys go false on this
    // very pump. But the server does not learn that until the next tick runs
    // Input.tick() -> sendMovementPackets. The equip wait below counts PUMP
    // steps, and this pump is a free-running 50 ms accumulator that is not
    // phase-locked to the game tick — so when a step lands just after a tick,
    // the whole wait can elapse before the next one, and the ClickSlot
    // overtakes the PlayerInput(false) it was supposed to follow. The server
    // still believes input=true and reads a container action taken mid-stride.
    // It only bites when the player was genuinely moving, which is why it
    // showed up once in many re-totems rather than constantly.
    //
    // Counting here rather than after the equip wait is what keeps it free:
    // both counters advance on the same pumps, so in the common case the gate
    // is already satisfied on the pump that clicks and costs nothing.
    //
    // Ordering is what makes two observations sufficient. This pump runs from
    // swapBuffers, i.e. at the END of runTick(), so any tick in the same frame
    // has already run. Two idle observations therefore span a real tick
    // boundary, and that tick saw the keys after unpressAll().
    if (movement_is_idle(env, c, mc)) ++g_idle_ticks;
    else                              g_idle_ticks = 0;

    if (g_equip_clock < g_equip_target_ticks) { ++g_equip_clock; return; }

    if (g_idle_ticks < REQUIRED_IDLE_TICKS) {
        // Never wedge. With a screen open the keys cannot come back on their
        // own, so if this never settles something upstream is wrong — say so
        // and go anyway rather than sit on an un-totemed player.
        if (++g_hold_ticks < MAX_HOLD_TICKS) {
            AT_TRACE("inventory", "holding for stillness: idle={} hold={}",
                     g_idle_ticks, g_hold_ticks);
            return;
        }
        VLOG_WARN("autototem", "stillness gate never settled — clicking anyway");
    }

    if (!g_sent) {
        // Revalidate the latch before acting on it. Clicking a slot that no
        // longer holds a totem swaps the wrong item into the off-hand, and the
        // server reads that as an invalid totem interaction.
        if (g_chosen_slot >= 0 && !slot_still_holds_totem(env, c, player.get(), g_chosen_slot))
            g_chosen_slot = -1;
        if (g_chosen_slot < 0) g_chosen_slot = find_totem_in_inventory(env, c, player.get());
        if (g_chosen_slot < 0) return;

        // Last-moment recheck. The offhand test at the top of this function ran
        // before the reaction and equip waits; a totem can have landed since
        // (a queued click resolving late, the player equipping one). Swapping
        // a totem OUT of a filled off-hand is exactly the nonsense interaction
        // the server would reject.
        if (offhand_has_totem(env, c, player.get())) return;

        const int this_latency = g_reaction_accum_ms + g_equip_target_ticks * TICK_MS;
        if (defer_for_collision(this_latency)) return;

        if (click_swap_to_offhand(env, c, mc, player.get(), g_chosen_slot)) {
            g_sent            = true;
            g_resend_clock    = 0;
            g_last_latency_ms = this_latency;
        }
    } else if (++g_resend_clock > RESEND_TICKS) {
        g_sent         = false;
        g_resend_clock = 0;
        g_chosen_slot  = -1;
    }
}

// ── SILENT MODE ──────────────────────────────────────────────────────────────
static void step_silent(JNIEnv* env, const tb::jni_cache& c, jobject mc, LocalRef& player)
{
    const bool offhand_full = offhand_has_totem(env, c, player.get());
    if (offhand_full) {
        if (g_active) {
            // The close already went out alongside the click — see the send
            // site below. This branch only tidies up. A session still marked
            // live here means the cycle ended without ever clicking (the totem
            // arrived some other way), so close it now for symmetry.
            if (g_silent_session) {
                send_close_screen_packet(env, c, player.get());
                g_silent_session = false;
            }
            reset();
        }
        return;
    }

    // A real screen means the player is doing something themselves.
    if (screen_is_open(env, c, mc)) { if (g_active) reset(); return; }

    if (!g_active) {
        if (find_totem_in_hotbar(env, c, player.get()) < 0 &&
            find_totem_in_inventory(env, c, player.get()) < 0) return;
        g_active = true;
        sample_reaction_delay();
        g_silent_session = false;
    }

    if (g_reaction_accum_ms < g_reaction_target_ms) {
        g_reaction_accum_ms += TICK_MS;
        return;
    }

    if (!g_silent_session) {
        g_silent_session = true;
        g_equip_clock    = 0;
    }

    // Stand still, the way opening a real inventory would make you.
    //
    // Two halves, and the second is what was missing: forcing the KeyBinding
    // to false is undone almost immediately by a Windows key-REPEAT, because
    // this pump runs inside the same call where glfwPollEvents drains those
    // events. Raising the filter drops the repeats at the window so the zero
    // survives to the tick that reads it. Without it Silent never actually
    // stopped the player, and the click went out mid-stride — MultiActions.
    hold_movement_keys(env, c, mc);
    lock_input_for(POST_ACTION_LOCK_TICKS, LOCK_FULL);

    if (g_equip_clock < g_equip_target_ticks) { ++g_equip_clock; return; }

    // Verify the player really has stopped, and has been stopped across a tick
    // boundary — "idle right now" is worthless on the tick we zeroed the keys
    // ourselves, because the read-back trivially agrees with the write while
    // the SERVER still holds the last input it was sent. Vanilla only emits a
    // new PlayerInput when the input CHANGES, i.e. on the next tick, so the
    // click has to wait for that boundary or it overtakes the packet saying we
    // stopped.
    if (movement_is_idle(env, c, mc)) ++g_idle_ticks;
    else                              g_idle_ticks = 0;

    if (g_idle_ticks < REQUIRED_IDLE_TICKS) {
        if (++g_hold_ticks < MAX_HOLD_TICKS) {
            AT_TRACE("silent", "holding: idle={} hold={} lock={}",
                     g_idle_ticks, g_hold_ticks, g_lock_level);
            return;
        }
        // Never wedge. If the keys refuse to go quiet, the suppression is not
        // taking on this host — say so loudly rather than sit here forever.
        VLOG_WARN("autototem", "movement never went idle after {} ticks — "
                               "suppression is not taking effect", g_hold_ticks);
    }
    g_hold_ticks = 0;

    if (!g_sent) {
        // Revalidate the latch before acting on it. Clicking a slot that no
        // longer holds a totem swaps the wrong item into the off-hand, and the
        // server reads that as an invalid totem interaction.
        if (g_chosen_slot >= 0 && !slot_still_holds_totem(env, c, player.get(), g_chosen_slot))
            g_chosen_slot = -1;
        // Re-evaluate priority after the waits, as the inventory may have changed.
        const int hotbar = find_totem_in_hotbar(env, c, player.get());
        const auto action = tb::silentTotemPlan(offhand_has_totem(env, c, player.get()),
            hotbar, hotbar < 0 ? find_totem_in_inventory(env, c, player.get()) : -1);
        g_chosen_slot = action.source;
        if (g_chosen_slot < 0) { reset(); return; }

        // Last-moment recheck. The offhand test at the top of this function ran
        // before the reaction and equip waits; a totem can have landed since
        // (a queued click resolving late, the player equipping one). Swapping
        // a totem OUT of a filled off-hand is exactly the nonsense interaction
        // the server would reject.


        const int this_latency = g_reaction_accum_ms + g_equip_target_ticks * TICK_MS;
        if (defer_for_collision(this_latency)) return;

        const int container_slot = action.containerSlot();
        const int button = action.button;
        if (click_swap_to_offhand(env, c, mc, player.get(), container_slot, button)) {
            g_sent            = true;
            g_resend_clock    = 0;
            g_last_latency_ms = this_latency;

            // Close the session IMMEDIATELY, in the same breath as the click.
            //
            // TotemGuard's AutoTotemF arms a 1500 ms timer whenever a totem is
            // clicked in the player inventory, and InventoryCloseEvent is what
            // DISARMS it. Any world interaction while it is still armed fails
            // the check — which is why a left/right click right after a
            // retotem flagged. A real player cannot hit that window at all:
            // with a screen open they cannot touch the world, so their close
            // always lands first.
            //
            // Deferring this to the end of the cycle (when the off-hand check
            // notices the totem) left the timer armed for a tick or more, and
            // that gap is precisely where the player's next click lands.
            send_close_screen_packet(env, c, player.get());
            g_silent_session = false;

            const int lo = tb::settings::silent_move_min_ms.load(std::memory_order_relaxed);
            const int hi = std::max(lo, tb::settings::silent_move_max_ms.load(std::memory_order_relaxed));
            std::uniform_int_distribution<int> d(lo, hi);
            g_silent_tail_ticks = std::max(1, d(rng()) / TICK_MS);
        }
    } else if (++g_resend_clock > RESEND_TICKS) {
        g_sent         = false;
        g_resend_clock = 0;
        g_chosen_slot  = -1;
    }
}

// ── dispatch ─────────────────────────────────────────────────────────────────
static void step(JNIEnv* env, const tb::jni_cache& c, jobject mc)
{
    // Dt's ClickGui is an ImGui surface drawn inside the GL frame, NOT a
    // Minecraft Screen — currentScreen stays null while it is open, so the
    // screen checks below cannot see it.
    if (tb::settings::overlay_open.load(std::memory_order_relaxed)) return;

    LocalRef player(env, env->GetObjectField(mc, c.mc_player_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!player) { if (g_active) reset(); return; }

    switch (static_cast<tb::Mode>(tb::settings::mode.load(std::memory_order_relaxed))) {
    case tb::Mode::Hotbar:
        // Hotbar drives vanilla's F keybind, and handleKeybinds does not run
        // its swap branch while a screen is up.
        if (screen_is_open(env, c, mc)) return;
        step_hotbar(env, c, mc, player);
        break;
    case tb::Mode::Silent:
        step_silent(env, c, mc, player);
        break;
    default:
        step_inventory(env, c, mc, player);
        break;
    }
}

// ── the pump ─────────────────────────────────────────────────────────────────
void pump(float delta_ms)
{
    if (tb::g_shutting_down.load(std::memory_order_acquire)) return;

    const bool module_on = tb::settings::enabled.load(std::memory_order_relaxed);
    const auto mode_now  =
        static_cast<tb::Mode>(tb::settings::mode.load(std::memory_order_relaxed));

    // The filter is a countdown that every reason to hold it re-extends. If no
    // reason extended it this pump, it expires on its own — so it can never
    // outlive its cause and leave the player without mouse or keyboard.
    // Switching the module off drops it immediately, whatever the count says.
    if (g_lock_level != LOCK_OFF) {
        if (!module_on) unlock_input();
        else if (--g_lock_hold_ticks <= 0) unlock_input();
    }

    // Throttle to the world-tick rate. Exactly one step per call — a catch-up
    // burst would collapse the delays the state machine is built on.
    static double accum_ms = 0.0;
    accum_ms += (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.0;
    if (accum_ms < TICK_MS) return;
    accum_ms -= TICK_MS;
    if (accum_ms > TICK_MS) accum_ms = TICK_MS;

    auto& c = tb::mutable_cache();
    if (!c.initialized || !c.minecraft_class || !c.mc_player_fid) return;

    JNIEnv* env = dt::jvm::thisThreadEnv();
    if (!env) return;

    LocalRef mc(env, resolve_minecraft(env, c));
    if (!mc) return;

    // Movement-suppression safety net. Anything that leaves the player unable
    // to walk must be undone by the very next pump, unconditionally — the
    // module being switched off is not an excuse to keep their keys.
    if ((!module_on || mode_now != tb::Mode::Silent)
        && (g_silent_session || g_silent_tail_ticks > 0))
    {
        // Just STOP holding. Writing anything back would press keys the player
        // is not holding — see hold_movement_keys. Key-repeat restores the
        // real state on its own within a frame or two.
        g_silent_session    = false;
        g_silent_tail_ticks = 0;
    }

    if (!module_on) {
        if (g_active) reset();
        return;
    }

    // Silent's movement tail: the session is over and the cycle already reset,
    // but the player must not snap back into a sprint on the same tick.
    if (g_silent_tail_ticks > 0) {
        hold_movement_keys(env, c, mc.get());
        lock_input_for(2, LOCK_FULL);   // keep filtering repeats through the tail
        // On the last tail tick we simply stop holding. Nothing is written
        // back: key-repeat re-presses whatever is genuinely still down, which
        // is the only source that knows the true state.
        --g_silent_tail_ticks;
    }

    // One-time reflective keybind lookup; needs a live GameOptions.
    if (!c.keybinds_resolved && c.mc_options_fid) {
        LocalRef options(env, env->GetObjectField(mc.get(), c.mc_options_fid));
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (options) resolve_keybinds(env, c, options.get());
    }

    step(env, c, mc.get());
}

} // namespace dt::modules::autototem_tick
