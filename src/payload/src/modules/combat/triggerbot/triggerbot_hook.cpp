#include "recharge_logic.h"
// triggerbot_hook.cpp — native triggerbot per-tick logic + JNIHooks.
//
// Two hooks:
//   • ClientPlayerEntity.tickMovement → validate gates + dispatch a synthetic
//     left-click on a confirmed crosshair target.
//   • MinecraftClient.doAttack        → gate the synthetic click at the exact
//     fire moment (anti air-swing) + attack bookkeeping.
//
// Dispatch is a REAL SendInput left-click so the game's own un-hooked
// handleInputEvents fires doAttack() at its natural frame phase → genuine
// vanilla swing animation, no desync, no missed hits, manual clicks untouched.

#include "triggerbot_hook.h"
#include "pending_click_logic.h"
#include "cooldown_logic.h"
#include "critical_logic.h"
#include "weapon_logic.h"
#include "triggerbot.h"
#include "focus_visual_logic.h"
#include "modules/combat/silentaim/silentaim.h"   // pre-tick anchor, see hkHandleBlockBreaking
#include "modules/combat/antiaction/antiaction.h"  // sword-into-shield gate, see hkDoAttack
#include "modules/shared/movement_idle.h"         // sprint reset, see hkDoAttack
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "platform/Win32Process.h"
#include "platform/Win32Window.h"
#include "core/social/EntityFriends.h"   // a friend is not a target
#include "modules/combat/backtrack/backtrack.h"   // ghost hitbox distance, see target_passes_validation
#include "util/Log.h"
#include "vgui/bridge.h"   // Freecam stand-down, see freecam_active
#include <sdk/mappings/mappings.hpp>
#include <jnihook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <utility>

namespace vanta {
namespace modules {
namespace triggerbot_hook {

// ─────────────────────────────────────────────────────────────────────────────
// log_(fmt, ...) — writes to a plain .txt on the user's desktop.
//
// Where it lands is figured out at DLL load, in this order:
//   1. C:\Users\vldwd\Desktop\vanta_trigger.txt   (classic Desktop)
//   2. C:\Users\vldwd\OneDrive\Desktop\vanta_trigger.txt
//                                                (OneDrive-redirected Desktop —
//                                                 this is why the file "wasn't
//                                                 appearing" for the user)
//   3. %USERPROFILE%\Desktop\vanta_trigger.txt   (whatever the profile says)
//   4. %TEMP%\vanta_trigger.txt                  (guaranteed writable fallback)
//
// The first fopen_s that succeeds wins. The final path is written back to
// disk in the header line so you can see WHERE the log actually is by
// opening whichever candidate you can find.
//
// One file per DLL session: opened with "w", so a fresh launch truncates.
// Every line is prefixed with a ms timestamp relative to the header line.
//
// A static constructor forces the open to happen at DLL load, not lazily
// on the first log_ call — this way the file APPEARS before the module is
// even enabled, so the user can confirm the plumbing is alive.
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex          g_log_mx;
static FILE*               g_log_fp   = nullptr;
static unsigned long long  g_log_t0   = 0;
static bool                g_log_open_tried = false;
static char                g_log_path[512] = {0};

static bool try_open_at(const char* path) {
    FILE* fp = nullptr;
    fopen_s(&fp, path, "w");
    if (!fp) return false;
    g_log_fp = fp;
    std::snprintf(g_log_path, sizeof(g_log_path), "%s", path);
    return true;
}

static void log_open_once() {}
namespace { struct _log_boot { _log_boot() {} } _log_boot_inst; }

static void log_(const char* fmt, ...) {
    std::lock_guard<std::mutex> _lk(g_log_mx);
    log_open_once();
    if (!g_log_fp) return;

    unsigned long long dt = GetTickCount64() - g_log_t0;
    std::fprintf(g_log_fp, "[%8llu ms] ", dt);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    std::fputc('\n', g_log_fp);
    std::fflush(g_log_fp);
}

// Exposed so triggerbot.cpp's run() heartbeat can log to the same file. The
// header declares it below in namespace vanta::modules::triggerbot_hook so
// callers see it too.
extern "C" void vanta_trigger_desktop_log(const char* fmt, ...) {
    std::lock_guard<std::mutex> _lk(g_log_mx);
    log_open_once();
    if (!g_log_fp) return;
    unsigned long long dt = GetTickCount64() - g_log_t0;
    std::fprintf(g_log_fp, "[%8llu ms] ", dt);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log_fp, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log_fp);
    std::fflush(g_log_fp);
}

// ─────────────────────────────────────────────────────────────────────────────
// TB_DBG(tag, fmt, ...)
//
// Loud diagnostic logger. Each call site is identified by `tag`, a string
// literal pointer. The FIRST call for a tag always prints. After that, the
// same tag re-prints at most once every 500 ms — that keeps a hot-loop
// gate (which fires 240× per second) from flooding the log while still
// giving a heartbeat you can grep for. Grep '[trigger] TB:' in vanta.log.
//
// Every diagnostic point in the trigger flow goes through this so you can
// see EXACTLY where the pipeline dies: run() entry, tick_from_render()
// entry, each of the ~12 gates inside try_trigger_attack, and the actual
// SendInput call. Turn debug off later by defining VANTA_TB_QUIET.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef VANTA_TB_QUIET
#define TB_DBG(tag, fmt_str, ...)                                          \
    do {                                                                   \
        static std::atomic<unsigned long long> _tb_next_ms{0};             \
        unsigned long long _tb_now  = GetTickCount64();                    \
        unsigned long long _tb_prev = _tb_next_ms.load(std::memory_order_relaxed); \
        if (_tb_now >= _tb_prev) {                                         \
            _tb_next_ms.store(_tb_now + 500, std::memory_order_relaxed);   \
            log_("TB[" tag "]: " fmt_str, ##__VA_ARGS__);                  \
        }                                                                  \
    } while (0)
// One-shot variant — logs the FIRST N times, always. Use for install /
// attach paths that only run a handful of times.
#define TB_ONCE(tag, fmt_str, ...)                                         \
    do {                                                                   \
        static std::atomic<int> _tb_left{5};                               \
        if (_tb_left.load(std::memory_order_relaxed) > 0) {                \
            _tb_left.fetch_sub(1, std::memory_order_relaxed);              \
            log_("TB[" tag "]: " fmt_str, ##__VA_ARGS__);                  \
        }                                                                  \
    } while (0)
#else
#define TB_DBG(tag, ...)  ((void)0)
#define TB_ONCE(tag, ...) ((void)0)
#endif

static jmethodID  ORIG_tickMovement    = nullptr;
static jclass     g_clientplayer_class = nullptr;
static jmethodID  g_tick_method_id     = nullptr;

static jmethodID  ORIG_doAttack        = nullptr;
static jclass     g_minecraft_class    = nullptr;
static jmethodID  g_doattack_method_id = nullptr;

static jmethodID  ORIG_handleInputEvents = nullptr;
static jmethodID  g_input_method_id      = nullptr;

// v124b: hook de MinecraftClient.handleBlockBreaking(boolean) — method_1590.
// Se llama cada frame que el LMB está apretado. Envía START_DIGGING / block-
// break-progress packets cuando el crosshair REAL apunta a un bloque. Si
// silent aim está activo con target publicado, hacemos NO-OP para suprimir
// el START_DIGGING que dispararía RotationBreak / MultiActionsF en Grim.
static jmethodID  ORIG_handleBlockBreaking = nullptr;
static jmethodID  g_hbb_method_id          = nullptr;

static bool       g_attached           = false;

static ULONGLONG  g_last_attack_time_ms = 0;
static std::atomic<ULONGLONG> g_last_combat_time_ms{ 0 };
static std::atomic<ULONGLONG> g_last_moving_time_ms{ 0 };

// Timestamp of the last time the validation+dispatch ran from the render
// thread (wglSwapBuffers). The world-tick hook (hkTickMovement) checks this:
// if the render thread is alive and pumping recently, the tick steps aside
// (no double work). If the render has been silent for > RENDER_HEARTBEAT_MS,
// the tick takes over so the bot keeps working on Vulkan-only hosts or
// when the render hook never installed.
static std::atomic<unsigned long long> g_last_render_dispatch_ms{ 0 };
static constexpr unsigned long long    RENDER_HEARTBEAT_MS = 500;

// Tick-aligned vertical-state snapshot for the Prioritize-crits gate.
//
// Render rate reads velocity 60-360 times per second, but the server
// only sees one velocity per WORLD tick (20 Hz). If we let render rate
// decide "descending vs ascending", we send attacks based on a velocity
// the server hasn't acknowledged yet — server still thinks the player
// is ascending, so the hit lands as a NORMAL even though we wanted a
// crit. Symptom: at the jump apex / on the way up the bot fires normals.
//
// The fix is to anchor the crit decision to the velocity SAMPLED AT
// THE WORLD-TICK RATE — server-aligned. Render rate keeps target/aim
// reaction speed; the crit/normal decision uses this snapshot only.
//
// Sample point semantics
// ----------------------
// The old build hooked tickMovement and sampled BEFORE the original
// ran, i.e. at the START of the tick — that vy is what the server
// already knows from the previous tick. The current build samples
// from sendMovementPackets, which runs AT THE END of the tick — vy
// has had this tick's gravity already applied (≈ 0.08 lower than what
// the server has in its simulation).
//
// To keep the crit gate aligned with what the server SEES rather than
// what the client computes locally, the DESCENDING threshold below is
// shifted by exactly one tick of gravity. Net behaviour: the gate
// fires the trigger only when the client is already past the
// apex+1-tick — the moment the server has also visibly transitioned
// to negative motion-Y. This is what made the old build "prioritize
// crits perfectly".
//
// Three states, no APEX bucket: with the gravity-shifted threshold,
// every frame between the jump start and one tick after the apex is
// classified ASCENDING (= skip). The first frame the client reports
// vy < -GRAVITY_PER_TICK is the same frame the server's own simulation
// will see vy < 0 next tick → safe to fire.
static std::atomic<int>  g_tick_vert_state{ 1 };
static constexpr int     TVS_ASCENDING  = 0;
static constexpr int     TVS_GROUND     = 1;
static constexpr int     TVS_DESCENDING = 2;

// Vanilla MC gravity is 0.08 b/tick; after the per-tick drag multiplier
// (0.98) it settles at ≈0.0784. We use 0.0784 so the threshold matches
// the actual delta the server sees between consecutive tick velocities.
// A small EPS on top guards against jitter in the rare case vy lands
// exactly on the threshold.
static constexpr double  GRAVITY_PER_TICK = 0.0784;
static constexpr double  VERT_EPS         = 1e-4;
static constexpr double  DESCENT_VELOCITY_THRESHOLD = -0.035; // Instant descent frame past apex

static constexpr unsigned long long POST_USE_DELAY_MS = 20;
static bool                  g_was_using_item_last_tick = false;
static unsigned long long    g_post_use_release_ms      = 0;
static unsigned long long    g_using_item_start_ms      = 0;
static bool                  g_was_eating               = false;
static unsigned long long    g_post_eat_release_ms      = 0;
static unsigned long long    g_ground_contact_ms        = 0;

static std::atomic<unsigned int> g_pending_on_hit{ 0 };
static std::atomic<unsigned int> g_pending_wtap_hit{ 0 };

// Tracks a synthetic left-click we injected via SendInput. When the game's
// natural handleInputEvents turns that click into a doAttack() call, hkDoAttack
// uses this to validate (and if needed suppress) ONLY our click — never the
// player's manual clicks. Set when we SendInput, consumed in hkDoAttack.

// v124: silent aim publica su target aquí. hkDoAttack lo lee (peek, sin
// consumir) durante el próximo attack — spoofea Minecraft.crosshairTarget
// al target del silent aim antes de llamar al doAttack original → hit
// vanilla al enemigo del silent aim sin hook propio del silent aim.
// Ambos silent aim tick_dispatch y hkDoAttack corren en el GAME THREAD →
// no hay concurrencia real. Silent aim maneja el lifecycle (DeleteGlobalRef
// al reemplazar via set_silentaim_target).
static std::atomic<jobject> g_silentaim_spoof_target{ nullptr };
// v124b: flag persistente separado del ref — hkHandleBlockBreaking lo lee
// para NO-OP el path de "start digging block" cuando silent aim tiene target
// activo. Esto suprime el packet START_DIGGING que dispararia RotationBreak
// y MultiActionsF en Grim al hacer click sobre un bloque con silent aim on.
static std::atomic<bool>    g_silentaim_active{ false };
// When set together with a synthetic click, hkDoAttack turns that click
// into a pure arm-swing (miss): it plays swingHand(MAIN_HAND) for the
// animation, then suppresses the real attack so no hit registers. This
// guarantees the miss while routing the swing through the exact same
// game-thread path/timing as a real attack — so the animation is a
// normal single swing, never the rapid mining-style swings the old
// render-thread swingHand produced.
// How long after injecting the click we still treat the next doAttack as ours.
static constexpr unsigned long long    SYNTHETIC_CLICK_WINDOW_MS = 200;
static std::mutex g_synthetic_click_mx;
static std::mutex g_recharge_mx;
static triggerbot::RechargeConfirmation g_recharge;
static triggerbot::pending_click::State g_synthetic_click;

static bool synthetic_click_pending() {
    std::lock_guard lock(g_synthetic_click_mx);
    return g_synthetic_click.pending(GetTickCount64(), SYNTHETIC_CLICK_WINDOW_MS);
}

static std::atomic<unsigned long long> g_dbg_hook_fires        { 0 };
static std::atomic<unsigned long long> g_dbg_attacks           { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_disabled  { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_no_target { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_validation{ 0 };
static std::atomic<unsigned long long> g_dbg_skipped_cooldown  { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_crit      { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_shield    { 0 };
static std::atomic<unsigned long long> g_dbg_skipped_using_item{ 0 };
static std::atomic<unsigned long long> g_dbg_skipped_post_use  { 0 };
static std::atomic<unsigned long long> g_dbg_dispatch_failed   { 0 };
static std::atomic<unsigned long long> g_dbg_last_attack_ms    { 0 };

// ── Anti-detection state ────────────────────────────────────────────────────
static int                g_consecutive_hits        = 0;
static std::atomic<double> g_current_random_offset{0.0};
static double             g_focus_level             = 1.0;
static jobject            g_focused_entity          = nullptr;
static jobject            g_focused_world           = nullptr;
static std::mutex         g_visual_focus_mx;
static jobject            g_visual_focus_entity       = nullptr;
static ULONGLONG          g_visual_focus_started_ms   = 0;
static jobject            g_last_target             = nullptr;
static ULONGLONG          g_target_acquired_ms      = 0;
static std::mt19937       g_rng{ std::random_device{}() };

// Miss chance refractory anchor. When the dice rolls "miss" we set
// this to a future timestamp; until that timestamp every fire attempt
// returns silently (no SendInput, no animation, no packet). Mimics the
// 180-360 ms reaction time a human takes to retry after a mis-clicked
// hit. Set to 0 = no active refractory.
static uint64_t           g_miss_until_ms           = 0;

// Combat and visual target references share a lock because shutdown can run
// concurrently with the render thread. Borrow local references before JNI use.
static void clear_focused_entity(JNIEnv* env, jobject expected = nullptr)
{
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    if (expected && g_focused_entity && env &&
        env->IsSameObject(expected, g_focused_entity) != JNI_TRUE) return;
    if (g_focused_entity && env) env->DeleteGlobalRef(g_focused_entity);
    g_focused_entity = nullptr;
    if (g_focused_world && env) env->DeleteGlobalRef(g_focused_world);
    g_focused_world = nullptr;
}

static jobject borrow_focused_entity(JNIEnv* env)
{
    if (!env) return nullptr;
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    return g_focused_entity ? env->NewLocalRef(g_focused_entity) : nullptr;
}

static void clear_visual_focus(JNIEnv* env)
{
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    if (g_visual_focus_entity && env) env->DeleteGlobalRef(g_visual_focus_entity);
    g_visual_focus_entity = nullptr;
    g_visual_focus_started_ms = 0;
}

static void refresh_visual_focus(JNIEnv* env, jobject target)
{
    if (!env || !target) return;
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    if (!g_visual_focus_entity || env->IsSameObject(g_visual_focus_entity, target) != JNI_TRUE) {
        jobject next = env->NewGlobalRef(target);
        if (!next) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
        if (g_visual_focus_entity) env->DeleteGlobalRef(g_visual_focus_entity);
        g_visual_focus_entity = next;
    }
    g_visual_focus_started_ms = GetTickCount64();
}

static void clear_last_target(JNIEnv* env)
{
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    if (g_last_target && env) env->DeleteGlobalRef(g_last_target);
    g_last_target = nullptr;
    g_target_acquired_ms = 0;
}

class LocalRef
{
public:
    LocalRef(JNIEnv* env, jobject ref) noexcept : env_(env), ref_(ref) {}
    ~LocalRef() { reset(); }
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    LocalRef(LocalRef&& o) noexcept : env_(o.env_), ref_(o.ref_) { o.ref_ = nullptr; }

    jobject get() const noexcept { return ref_; }
    operator jobject() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }

    void reset() noexcept
    {
        if (env_ && ref_) { env_->DeleteLocalRef(ref_); ref_ = nullptr; }
    }

private:
    JNIEnv* env_;
    jobject ref_;
};

// Randomized cooldown offset: sinusoidal focus level + Gaussian + exponential
// + velocity/air adjustments + rare spikes. Non-deterministic attack timing.
static void generate_next_offset(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!triggerbot::settings::randomize_cooldown)
    {
        g_current_random_offset = 0.0;
        return;
    }

    ULONGLONG now = GetTickCount64();
    double t = now / 1000.0;

    double noise = std::sin(t * 0.5) + std::sin(t * 0.8 * 3.14159265358979323846)
                 + std::sin(t * 1.3 * 2.71828182845904523536);
    g_focus_level = (std::max)(0.3, (std::min)(1.0, 0.65 + (noise / 6.0)));

    double fatigue = (std::min)(1.0, g_consecutive_hits / 15.0);
    double mu      = 0.0;
    double sigma   = 0.5 + (1.0 - g_focus_level) * 0.8 + fatigue * 0.5;
    double tau     = 0.5 + (1.0 - g_focus_level) * 1.0 + fatigue * 0.5;

    std::normal_distribution<double> gauss(mu, sigma);
    double gaussian_part = gauss(g_rng);

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    double u = 1.0 - uni(g_rng);
    if (u <= 0.0) u = 1e-10;
    double exponential_part = -std::log(u) * tau;

    double final_offset = gaussian_part + exponential_part - (tau * 0.5);

    if (local_player && c.entity_velocity_fid)
    {
        LocalRef vel(env, env->GetObjectField(local_player, c.entity_velocity_fid));
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (vel && c.vec3d_class)
        {
            static jfieldID vx_fid = nullptr;
            static jfieldID vz_fid = nullptr;
            if (!vx_fid)
            {
                vx_fid = env->GetFieldID(c.vec3d_class, "field_1352", "D");
                vz_fid = env->GetFieldID(c.vec3d_class, "field_1350", "D");
                if (env->ExceptionCheck()) { env->ExceptionClear(); vx_fid = nullptr; vz_fid = nullptr; }
            }
            if (vx_fid && vz_fid)
            {
                double vx = env->GetDoubleField(vel, vx_fid);
                double vz = env->GetDoubleField(vel, vz_fid);
                double speed = std::sqrt(vx * vx + vz * vz);
                if (speed > 0.15) final_offset += uni(g_rng);
            }
        }
        if (c.entity_is_on_ground_mid)
        {
            jboolean on_ground = env->CallBooleanMethod(local_player, c.entity_is_on_ground_mid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (on_ground == JNI_FALSE) final_offset += uni(g_rng) * 1.5;
        }
    }

    if (uni(g_rng) < 0.01)      final_offset += 2.5 + uni(g_rng) * 2.0;
    else if (uni(g_rng) < 0.02) final_offset -= 2.0 + uni(g_rng) * 2.0;

    g_current_random_offset = std::clamp(final_offset, 0.0, 1.0);
}

static bool cache_is_complete(const triggerbot::jni_cache& c)
{
    return c.initialized
        && c.minecraft_class
        && c.minecraft_player_fid
        && c.minecraft_crosshair_target_fid
        && c.entity_hit_result_class
        && c.entity_hit_result_get_entity_mid;
}

static jobject resolve_minecraft(JNIEnv* env, const triggerbot::jni_cache& c)
{
    jfieldID instance_fid = env->GetStaticFieldID(c.minecraft_class,
        sdk::mappings::minecraftclient_name,
        sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!instance_fid) return nullptr;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return mc;
}

static jobject resolve_crosshair_entity(JNIEnv* env,
    const triggerbot::jni_cache& c, jobject minecraft)
{
    jobject hit = env->GetObjectField(minecraft, c.minecraft_crosshair_target_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!hit) return nullptr;

    if (env->IsInstanceOf(hit, c.entity_hit_result_class) != JNI_TRUE)
    {
        env->DeleteLocalRef(hit);
        return nullptr;
    }

    jobject entity = env->CallObjectMethod(hit, c.entity_hit_result_get_entity_mid);
    env->DeleteLocalRef(hit);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return entity;
}

// ─────────────────────────────────────────────────────────────────────────────
// Extended-reach entity raycast.
//
// Minecraft.crosshairTarget only ever fills in entities within the game's
// built-in interaction reach (≈3 blocks in survival). A `attack_range`
// slider set to 4/5/6 blocks was silently no-oping past 3 because the
// vanilla pick never produced an EntityHitResult that far out. This helper
// walks the ClientWorld's rendered entities, computes an AABB from each
// entity's position (0.6 wide × 1.8 tall, matching a player hitbox — the
// dominant PvP case), and does a slab-method ray/box intersection against a
// ray fired from the player's eyes along the current view vector. The
// closest hit within `reach` is returned; nullptr if nothing is under the
// crosshair inside that budget.
//
// Returns a LOCAL reference the caller owns (DeleteLocalRef when done),
// or nullptr on any failure. Never throws; JNI exceptions are cleared.
// ─────────────────────────────────────────────────────────────────────────────
static jobject raycast_extended_reach(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject minecraft, jobject local_player, double reach)
{
    if (!minecraft || !local_player) return nullptr;
    if (!c.minecraft_world_fid || !c.world_get_entities_mid) return nullptr;
    if (!c.iterable_iterator_mid || !c.iterator_has_next_mid || !c.iterator_next_mid) return nullptr;
    if (!c.entity_get_x_mid || !c.entity_get_y_mid || !c.entity_get_z_mid) return nullptr;
    if (!c.entity_yaw_fid || !c.entity_pitch_fid) return nullptr;

    // Ray origin: player eye position.
    double ex = env->CallDoubleMethod(local_player, c.entity_get_x_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!c.entity_get_eye_y_mid) return nullptr;
    double ey = env->CallDoubleMethod(local_player, c.entity_get_eye_y_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    double ez = env->CallDoubleMethod(local_player, c.entity_get_z_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    // Direction: from yaw + pitch (Minecraft convention).
    float yaw_deg   = env->GetFloatField(local_player, c.entity_yaw_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    float pitch_deg = env->GetFloatField(local_player, c.entity_pitch_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    double y_rad = yaw_deg   * kDegToRad;
    double p_rad = pitch_deg * kDegToRad;
    double cos_p = std::cos(p_rad);
    double dx =  -std::sin(y_rad) * cos_p;
    double dy =  -std::sin(p_rad);
    double dz =   std::cos(y_rad) * cos_p;

    // Fetch ClientWorld → iterator.
    LocalRef world(env, env->GetObjectField(minecraft, c.minecraft_world_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!world) return nullptr;

    LocalRef iterable(env, env->CallObjectMethod(world.get(), c.world_get_entities_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!iterable) return nullptr;

    LocalRef it(env, env->CallObjectMethod(iterable.get(), c.iterable_iterator_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!it) return nullptr;

    // The block ray follows the view ray; never attack if it cannot be read.
    if (!c.entity_block_raycast_mid || !c.hit_result_get_pos_mid ||
        !c.vec3d_x_fid || !c.vec3d_y_fid || !c.vec3d_z_fid) return nullptr;
    LocalRef hit(env, env->CallObjectMethod(local_player, c.entity_block_raycast_mid,
                                             static_cast<jdouble>(reach), 1.0f, JNI_FALSE));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!hit) return nullptr;
    LocalRef pos(env, env->CallObjectMethod(hit.get(), c.hit_result_get_pos_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!pos) return nullptr;
    const double hx = env->GetDoubleField(pos.get(), c.vec3d_x_fid) - ex;
    const double hy = env->GetDoubleField(pos.get(), c.vec3d_y_fid) - ey;
    const double hz = env->GetDoubleField(pos.get(), c.vec3d_z_fid) - ez;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    const double block_dist = std::sqrt(hx * hx + hy * hy + hz * hz);
    if (!std::isfinite(block_dist)) return nullptr;

    jobject best = nullptr;
    double best_t = reach + 1e-6;

    // Hitbox geometry — player-sized. Wider than crystals/small mobs but
    // narrower than a lot of hostile mobs; acceptable for the PvP case the
    // reach slider is aimed at.
    constexpr double half_w = 0.30;   // 0.60 wide
    constexpr double height = 1.80;

    for (;;)
    {
        jboolean has_next = env->CallBooleanMethod(it.get(), c.iterator_has_next_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (has_next != JNI_TRUE) break;

        LocalRef ent(env, env->CallObjectMethod(it.get(), c.iterator_next_mid));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!ent) continue;

        // Skip the local player itself and anything that isn't Living
        // (drops, XP orbs, arrows aren't attack targets).
        if (env->IsSameObject(ent.get(), local_player) == JNI_TRUE) continue;
        if (c.living_entity_class &&
            env->IsInstanceOf(ent.get(), c.living_entity_class) != JNI_TRUE) continue;

        double tx = env->CallDoubleMethod(ent.get(), c.entity_get_x_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        double ty = env->CallDoubleMethod(ent.get(), c.entity_get_y_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        double tz = env->CallDoubleMethod(ent.get(), c.entity_get_z_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

        // Cheap early reject: eye-to-entity squared distance vs reach.
        double ddx = tx - ex, ddy = (ty + height * 0.5) - ey, ddz = tz - ez;
        double sq = ddx * ddx + ddy * ddy + ddz * ddz;
        double budget = reach + 1.0; // + hitbox slop
        if (sq > budget * budget) continue;

        // Slab-method ray/AABB.
        double mn_x = tx - half_w, mx_x = tx + half_w;
        double mn_y = ty,          mx_y = ty + height;
        double mn_z = tz - half_w, mx_z = tz + half_w;

        double t_near = -1e30, t_far = 1e30;

        auto slab = [&](double o, double d, double lo, double hi) -> bool {
            if (std::fabs(d) < 1e-9) return (o >= lo && o <= hi);
            double t1 = (lo - o) / d;
            double t2 = (hi - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > t_near) t_near = t1;
            if (t2 < t_far)  t_far  = t2;
            return t_near <= t_far;
        };
        if (!slab(ex, dx, mn_x, mx_x)) continue;
        if (!slab(ey, dy, mn_y, mx_y)) continue;
        if (!slab(ez, dz, mn_z, mx_z)) continue;

        if (t_far < 0.0) continue;                       // box behind us
        double t = t_near < 0.0 ? 0.0 : t_near;         // inside box → 0
        if (t > best_t || t + 0.001 >= block_dist) continue;

        // Keep the closest hit. Replace the previous best local ref so we
        // don't leak across many candidates.
        if (best) env->DeleteLocalRef(best);
        best = env->NewLocalRef(ent.get());
        best_t = t;
    }

    return best;
}

static bool target_is_dead(JNIEnv* env, const triggerbot::jni_cache& c, jobject target)
{
    if (!target) return true;
    if (c.entity_is_spectator_mid) {
        jboolean spec = env->CallBooleanMethod(target, c.entity_is_spectator_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (spec == JNI_TRUE) return true;
    }
    if (c.entity_is_alive_mid) {
        jboolean alive = env->CallBooleanMethod(target, c.entity_is_alive_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (alive != JNI_TRUE) return true;
    }
    if (c.entity_is_removed_mid) {
        jboolean rem = env->CallBooleanMethod(target, c.entity_is_removed_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (rem == JNI_TRUE) return true;
    }
    if (c.living_entity_class && env->IsInstanceOf(target, c.living_entity_class) == JNI_TRUE) {
        if (c.living_entity_get_health_mid) {
            jfloat hp = env->CallFloatMethod(target, c.living_entity_get_health_mid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (hp <= 0.0f) return true;
        }
        jfieldID dtFid = env->GetFieldID(c.living_entity_class, "field_6213", "I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!dtFid) {
            dtFid = env->GetFieldID(c.living_entity_class, "deathTime", "I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (dtFid && env->GetIntField(target, dtFid) > 0) return true;

        jmethodID isDeadMid = env->GetMethodID(c.living_entity_class, "method_29504", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (isDeadMid && env->CallBooleanMethod(target, isDeadMid) == JNI_TRUE) return true;
    }
    if (c.entity_class) {
        jmethodID invulnMid = env->GetMethodID(c.entity_class, "method_5682", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!invulnMid) {
            invulnMid = env->GetMethodID(c.entity_class, "isInvulnerable", "()Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (invulnMid && env->CallBooleanMethod(target, invulnMid) == JNI_TRUE) return true;
    }
    if (c.player_class && env->IsInstanceOf(target, c.player_class) == JNI_TRUE) {
        jmethodID creativeMid = env->GetMethodID(c.player_class, "method_7337", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!creativeMid) {
            creativeMid = env->GetMethodID(c.player_class, "isCreative", "()Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (creativeMid && env->CallBooleanMethod(target, creativeMid) == JNI_TRUE) return true;

        jfieldID abFid = env->GetFieldID(c.player_class, "field_7503", "Lnet/minecraft/class_1656;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!abFid) {
            abFid = env->GetFieldID(c.player_class, "abilities", "Lnet/minecraft/class_1656;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (abFid) {
            jobject ab = env->GetObjectField(target, abFid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (ab) {
                jclass abCls = env->GetObjectClass(ab);
                jfieldID flyFid = env->GetFieldID(abCls, "field_7479", "Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (!flyFid) {
                    flyFid = env->GetFieldID(abCls, "flying", "Z");
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                jfieldID allowFlyFid = env->GetFieldID(abCls, "field_7478", "Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (!allowFlyFid) {
                    allowFlyFid = env->GetFieldID(abCls, "allowFlying", "Z");
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                const bool flying = (flyFid && env->GetBooleanField(ab, flyFid) == JNI_TRUE) ||
                                    (allowFlyFid && env->GetBooleanField(ab, allowFlyFid) == JNI_TRUE);
                env->DeleteLocalRef(abCls);
                env->DeleteLocalRef(ab);
                if (flying) return true;
            }
        }
    }
    return false;
}

// hurtTime gate: target is in damage immunity (invulnerable) if hurtTime > max_hurt_time.
// Default max_hurt_time = 0, meaning any hurtTime (ticks 1..10) blocks attack, strictly preventing freehits.
static bool target_in_invuln(JNIEnv* env, const triggerbot::jni_cache& c, jobject target)
{
    if (!c.living_entity_class || !c.living_entity_hurt_time_fid) return false;
    if (env->IsInstanceOf(target, c.living_entity_class) != JNI_TRUE) return false;
    jint hurt = env->GetIntField(target, c.living_entity_hurt_time_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    const int max_hurt = triggerbot::settings::max_hurt_time;
    return hurt > max_hurt;
}

static double eye_to_target_distance(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player, jobject target)
{
    if (!c.entity_get_x_mid || !c.entity_get_y_mid || !c.entity_get_z_mid) return -1.0;

    double px = env->CallDoubleMethod(local_player, c.entity_get_x_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }
    double py_eye = env->CallDoubleMethod(local_player, c.entity_get_y_mid) + 1.62;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }
    double pz = env->CallDoubleMethod(local_player, c.entity_get_z_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }

    double ex = env->CallDoubleMethod(target, c.entity_get_x_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }
    double ey_foot = env->CallDoubleMethod(target, c.entity_get_y_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }
    double ez = env->CallDoubleMethod(target, c.entity_get_z_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0; }

    constexpr double half_w = 0.3;
    constexpr double height = 1.8;

    double cx = (px < ex - half_w) ? (ex - half_w)
              : (px > ex + half_w) ? (ex + half_w) : px;
    double cy = (py_eye < ey_foot) ? ey_foot
              : (py_eye > ey_foot + height) ? (ey_foot + height) : py_eye;
    double cz = (pz < ez - half_w) ? (ez - half_w)
              : (pz > ez + half_w) ? (ez + half_w) : pz;

    double dx = px - cx, dy = py_eye - cy, dz = pz - cz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static bool focus_allows_target(JNIEnv* env, jobject target)
{
    if (triggerbot::settings::only_on_hit) {
        LocalRef focused(env, borrow_focused_entity(env));
        if (!focused) return false;
        return env->IsSameObject(target, focused.get()) == JNI_TRUE;
    }
    if (!triggerbot::settings::focus_mode) return true;
    LocalRef focused(env, borrow_focused_entity(env));
    return !focused || env->IsSameObject(target, focused.get());
}
static bool holding_weapon(JNIEnv* env, const triggerbot::jni_cache& c, jobject local_player)
{
    if (!c.player_inventory_fid || !c.inventory_selected_slot_fid ||
        !c.inventory_get_stack_mid || !c.itemstack_is_empty_mid ||
        !c.itemstack_get_item_mid || !c.item_get_translation_key_mid) return true;

    LocalRef inv(env, env->GetObjectField(local_player, c.player_inventory_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!inv) return true;

    jint slot = env->GetIntField(inv, c.inventory_selected_slot_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }

    LocalRef stack(env, env->CallObjectMethod(inv, c.inventory_get_stack_mid, slot));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!stack) return true;

    if (env->CallBooleanMethod(stack, c.itemstack_is_empty_mid) == JNI_TRUE) return false;

    LocalRef item(env, env->CallObjectMethod(stack, c.itemstack_get_item_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!item) return true;

    LocalRef key(env, env->CallObjectMethod(item, c.item_get_translation_key_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!key) return true;

    const char* k = env->GetStringUTFChars((jstring)key.get(), nullptr);
    bool is_weapon = false;
    if (k)
    {
        is_weapon = ::vanta::modules::triggerbot::is_weapon_key(k);
        env->ReleaseStringUTFChars((jstring)key.get(), k);
    }
    return is_weapon;
}

// Reads the local player's currently-selected hotbar slot (0-8).
// Returns -1 on error. Used by the slot-restriction gate.
static int read_selected_hotbar_slot(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!c.player_inventory_fid || !c.inventory_selected_slot_fid) return -1;
    LocalRef inv(env, env->GetObjectField(local_player, c.player_inventory_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (!inv) return -1;
    jint slot = env->GetIntField(inv, c.inventory_selected_slot_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (slot < 0 || slot > 8) return -1;
    return (int)slot;
}

// Returns true when slot restriction is enabled AND the user's currently
// selected hotbar slot is NOT enabled in the slot mask. Caller skips
// firing in that case. When the option is off, always returns false.
static bool slot_restricted(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!triggerbot::settings::slot_restrict) return false;
    int slot = read_selected_hotbar_slot(env, c, local_player);
    if (slot < 0) return false;
    int mask = triggerbot::settings::slot_mask & 0x1FF;
    return (mask & (1 << slot)) == 0;
}

static bool target_passes_validation(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player, jobject target)
{
    // A friend is not a target — checked first, before anything that costs JNI.
    if (::vanta::social::isFriend(env, target)) return false;

    // A teammate is not a target when check_teams is active.
    if (triggerbot::settings::check_teams && ::vanta::social::isTeammate(env, local_player, target))
        return false;

    if (target_in_invuln(env, c, target)) return false;

    // alive check is always on (no GUI knob)
    if (target_is_dead(env, c, target)) return false;

    if (triggerbot::settings::only_weapons && !holding_weapon(env, c, local_player))
        return false;

    // reach check: clamp to the user's configured attack range (vanilla cap 3).
    {
        double dist = eye_to_target_distance(env, c, local_player, target);
        // Backtrack: when the crosshair sits on this target's GHOST hitbox (the
        // spoofed crosshairTarget is what brought us here), the distance that
        // matters is the one to the ghost — the body itself may already have
        // run past the range, and measuring it is what made the bot go quiet
        // on every backtracked hit.
        const double ghost = ::vanta::modules::backtrack::ghost_hit_distance(env, target);
        if (ghost >= 0.0 && (dist < 0.0 || ghost < dist)) dist = ghost;
        if (dist < 0.0) return false;
        double reach = (double)triggerbot::settings::attack_range;
        if (reach <= 0.0) reach = 2.999;
        if (dist > reach) return false;
    }

    return true;
}

static float get_cooldown_progress(JNIEnv* env, const triggerbot::jni_cache& c, jobject local_player, float base_time = 0.5f)
{
    if (!c.player_get_attack_cooldown_progress_mid) return 0.0f;
    jfloat p = env->CallFloatMethod(local_player, c.player_get_attack_cooldown_progress_mid, base_time);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0.0f; }
    return (float)p;
}

static float configured_cooldown_target() {
    return triggerbot::cooldown::threshold(triggerbot::settings::cooldown_threshold,
        triggerbot::settings::randomize_cooldown, g_current_random_offset);
}

static bool player_on_ground(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!c.entity_is_on_ground_mid) return true;
    jboolean og = env->CallBooleanMethod(local_player, c.entity_is_on_ground_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    return og == JNI_TRUE;
}

static bool read_velocity_y(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player, double& out_vy);

static float effective_cooldown_target(bool grounded, double vy = 0.0) {
    if (!grounded && triggerbot::settings::wait_for_crit) {
        // Instant punish crits: target 0.910F so tick 11 (0.920F) is ready immediately.
        // In vanilla Minecraft, crits strictly require cooldownProgress > 0.90F.
        // Fires on the earliest descent frame without waiting for full charge (1.0F).
        const float crit_target = 0.910f;
        const float jitter = triggerbot::settings::randomize_cooldown
            ? static_cast<float>((g_rng() % 5) / 1000.0f) // 0.000 to 0.004 -> max 0.914f <= 0.920f
            : 0.0f;
        return crit_target + jitter;
    }

    if (grounded) {
        // Ground sweeps: varied cadence ("variado rapido lento")
        // Alternates dynamically between fast sweeps on tick 11 (0.911F - 0.920F)
        // and full slower sweeps on tick 12 (0.950F - 0.985F).
        if (triggerbot::settings::full_charge_normal) {
            return 0.98f;
        }
        if (triggerbot::settings::randomize_cooldown) {
            const int roll = g_rng() % 100;
            if (roll < 55) {
                // 55% quick sweeps on tick 11
                const float fast_jitter = static_cast<float>((g_rng() % 10) / 1000.0f); // 0.000 to 0.009 -> 0.911f to 0.920f
                return 0.911f + fast_jitter;
            } else {
                // 45% slower full sweeps on tick 12
                const float slow_jitter = static_cast<float>((g_rng() % 35) / 1000.0f); // 0.000 to 0.035 -> 0.950f to 0.985f
                return 0.950f + slow_jitter;
            }
        }
        return 0.912f; // Fast baseline without randomization
    }

    // Normal hits: fast, but strictly protected from weak 1.8 spam hits (<0.90F)
    float base = configured_cooldown_target();
    return std::clamp(base, 0.910f, 1.0f);
}

static bool cooldown_meets_threshold(JNIEnv* env, const triggerbot::jni_cache& c, jobject local_player,
                                    float reserved_target = 0.0f)
{
    const bool grounded = player_on_ground(env, c, local_player);
    double vy = 0.0;
    if (!grounded) {
        read_velocity_y(env, c, local_player, vy);
    }
    float target = effective_cooldown_target(grounded, vy);
    if (reserved_target > 0.0f) {
        if (!grounded && triggerbot::settings::wait_for_crit) {
            target = reserved_target;
        } else {
            target = std::max(target, reserved_target);
        }
    }
    // Vanilla Minecraft PlayerEntity.attack(Entity) evaluates with baseTime = 0.5F.
    // Sampling with base_time = 0.5f aligns ground hits and crits to exact server ticks,
    // allowing tick 12 ground sweeps (1.000F) and tick 11 air crits (0.920F > 0.90F)
    // to fire with zero wasted ticks in Sword Pot and Nethpot trades.
    const float base_time = 0.5f;
    const float progress = get_cooldown_progress(env, c, local_player, base_time);
    {
        std::lock_guard lock(g_recharge_mx);
        if (g_recharge.blocked(progress, GetTickCount64())) return false;
    }
    return triggerbot::cooldown::ready(progress, target);
}

static bool player_sprinting(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!c.entity_is_sprinting_mid) return false;
    jboolean sp = env->CallBooleanMethod(local_player, c.entity_is_sprinting_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return sp == JNI_TRUE;
}

static bool player_in_water(JNIEnv* env, const triggerbot::jni_cache& c, jobject local_player)
{
    if (!c.entity_is_touching_water_mid) return false;
    jboolean w = env->CallBooleanMethod(local_player, c.entity_is_touching_water_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return w == JNI_TRUE;
}

// Prioritize-crits descent detection.
//
// Single source of truth: velocity.y < 0 = descending. Used identically
// across every 1.21.x sub-version (1.21.0 .. 1.21.11) because the velocity
// vector field lives in the same place in every mapping flavour:
//
//   • Yarn:   class_1297.field_18276 of type class_243 (Vec3d) with
//             x = field_1352, y = field_1351, z = field_1350.
//   • Mojang: Entity.deltaMovement of type Vec3 with x/y/z.
//
// No fallback to fallDistance, no type probing, no version branches. If
// velocity.y can't be read (cache wasn't built) we treat it as "not
// descending" and the gate keeps holding — Prioritize crits is the
// strict mode by design, normal hits on the ground still go through.
//
// Behaviour the user asked for:
//   • Falling (vy < 0)              → crit at the configured cooldown.
//   • Jumping up / apex hold (vy>=0) → wait for the descent.
//   • On the ground                 → normal hits (handled in check_hit_gate).
// Reads vertical velocity (y component of the velocity Vec3d/Vec3 field).
// Returns true with vy filled in on success; false if anything in the chain
// is unavailable (caller should treat that as "unknown").
//
// Single shape across every 1.21.x sub-version + every mapping flavour:
//   • Yarn:   class_1297.field_18276 of class_243 with y = field_1351
//   • Mojang: Entity.deltaMovement of Vec3 with y
static bool read_velocity_y(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player, double& out_vy)
{
    out_vy = 0.0;
    if (!c.entity_velocity_fid || !c.vec3d_class || !c.vec3d_y_fid) return false;

    jobject vel = env->GetObjectField(local_player, c.entity_velocity_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!vel) return false;

    jdouble vy = env->GetDoubleField(vel, c.vec3d_y_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(vel); return false; }
    env->DeleteLocalRef(vel);

    out_vy = (double)vy;
    return true;
}

enum class HitGate { Allow, Skip };

static HitGate check_hit_gate(JNIEnv* env, const triggerbot::jni_cache& c, jobject player)
{
    if (triggerbot::settings::wait_for_crit) {
        if (!c.entity_is_on_ground_mid) return HitGate::Allow;
        const bool grounded = env->CallBooleanMethod(player, c.entity_is_on_ground_mid) == JNI_TRUE;
        if (env->ExceptionCheck()) { env->ExceptionClear(); return HitGate::Allow; }

        if (grounded) {
            // When on the ground:
            // If the player is holding Space (jumping for a crit), HOLD the ground hit
            // so we don't burn cooldown 1 tick before leaving the ground.
            // If we hit on the ground right as we jump, cooldown drops to 0% and the
            // jump cannot crit (wasted hit = "freehit").
            // Exception: if the player has been grounded for > 250ms (e.g. trapped under ceiling),
            // allow the ground hit so they don't get stuck.
            const bool space_down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            if (space_down) {
                const uint64_t now = GetTickCount64();
                if (g_ground_contact_ms == 0) g_ground_contact_ms = now;
                if (now - g_ground_contact_ms < 250) {
                    return HitGate::Skip; // Hold for the upcoming air crit!
                }
            } else {
                g_ground_contact_ms = 0;
            }
            return HitGate::Allow; // Normal ground hit with sweep
        }

        g_ground_contact_ms = 0;

        // In water: vanilla cancels crits anyway, allow normal hit
        if (player_in_water(env, c, player))
            return HitGate::Allow;

        // In the air (!grounded):
        // INSTANT PUNISH CRIT:
        // Fire on the very first sub-tick frame of descent (velocity <= -0.035) with positive
        // fall accumulation (fallDistance > 0.002) so vanilla Minecraft's condition
        // (fallDistance > 0.0F && !onGround) is 100% satisfied with ZERO delay!
        double velocity = 0.0;
        const bool has_vy = read_velocity_y(env, c, player, velocity) && std::isfinite(velocity);

        double fall = 0.0;
        bool has_fall_fid = false;
        if (c.entity_fall_distance_fid) {
            fall = sdk::mappings::fall_distance_is_float
                ? static_cast<double>(env->GetFloatField(player, c.entity_fall_distance_fid))
                : env->GetDoubleField(player, c.entity_fall_distance_fid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fall = 0.0; }
            else { has_fall_fid = true; }
        }

        const int vert_state = g_tick_vert_state.load(std::memory_order_relaxed);
        const bool is_descending = (has_vy && velocity <= DESCENT_VELOCITY_THRESHOLD) || (vert_state == TVS_DESCENDING);
        const bool fall_ready = !has_fall_fid || (fall > 0.002);

        if (is_descending && fall_ready) {
            return HitGate::Allow; // Instant Guaranteed Crit descent!
        }

        // Ascending, lingering at apex, or fallDistance not yet positive: hold!
        return HitGate::Skip;
    }
    return HitGate::Allow;
}
enum class UseGate { Allow, SkipUsing, SkipPostUse };

static UseGate gate_when_use_hand(JNIEnv* env, const triggerbot::jni_cache& c, jobject local_player)
{
    const unsigned long long now_ms = GetTickCount64();
    const bool right_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool left_down  = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    jboolean using_mid = JNI_FALSE;
    if (c.living_entity_is_using_item_mid)
    {
        using_mid = env->CallBooleanMethod(local_player, c.living_entity_is_using_item_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Actively using item: check game engine's isUsingItem() first; if cache unavailable fall back to right click.
    const bool now = (c.living_entity_is_using_item_mid ? (using_mid == JNI_TRUE) : (right_down && !left_down));
    const bool was = g_was_using_item_last_tick;
    g_was_using_item_last_tick = now;

    if (now)
    {
        if (g_using_item_start_ms == 0) g_using_item_start_ms = now_ms;
        // If item use is held for >= 180 ms (approx 3-4 ticks), this is an eating/channeling action
        if ((now_ms - g_using_item_start_ms) >= 180) {
            g_was_eating = true;
        }
        g_post_use_release_ms = now_ms;
        return UseGate::SkipUsing;
    }

    if (was)
    {
        // Transitioned from using -> stopped using
        g_post_use_release_ms = now_ms;
        if (g_was_eating) {
            g_post_eat_release_ms = now_ms;
            g_was_eating = false;
        }
        g_using_item_start_ms = 0;
    }

    // Post-eat refractory: after finishing or stopping a gapple/food action,
    // wait for post_eat_delay_ms (default ~100ms) before allowing an attack.
    // This allows the eating animation, sounds and particles to fully settle,
    // guaranteeing you never hit while still in the eating animation.
    if (g_post_eat_release_ms != 0)
    {
        const unsigned long long elapsed_eat = now_ms - g_post_eat_release_ms;
        const unsigned long long required_delay = static_cast<unsigned long long>(
            std::max(0.0f, static_cast<float>(triggerbot::settings::post_eat_delay_ms)));
        if (elapsed_eat < required_delay) return UseGate::SkipPostUse;
        g_post_eat_release_ms = 0;
    }

    // Standard fast post-use delay for instant actions like throwing splash potions (20ms)
    if (g_post_use_release_ms != 0)
    {
        const unsigned long long elapsed = now_ms - g_post_use_release_ms;
        if (elapsed < POST_USE_DELAY_MS) return UseGate::SkipPostUse;
        g_post_use_release_ms = 0;
    }

    return UseGate::Allow;
}

// "Work in screen": when off, don't fire while a GUI/screen is open.
static bool gate_attack_in_gui(JNIEnv* env, const triggerbot::jni_cache& c, jobject minecraft)
{
    // Checked FIRST, ahead of work_in_screen — a deliberate deviation from
    // enhance, which lets work_in_screen short-circuit this. The two flags
    // mean different things: "work in screens" is about MINECRAFT screens
    // (inventory, chests), while show_gui is our own ClickGui overlay. The
    // dispatch path is a real SendInput left-click, so firing with the panel
    // up clicks the panel — toggling modules and dragging sliders under the
    // user's cursor. No user setting should be able to opt into that.
    if (triggerbot::settings::show_gui) return false;

    if (triggerbot::settings::work_in_screen) return true;

    if (c.minecraft_current_screen_fid)
    {
        LocalRef screen(env, env->GetObjectField(minecraft, c.minecraft_current_screen_fid));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        if (screen) return false;
    }
    return true;
}

// vanta-c target type: 0 = All, 1 = Players, 2 = Mobs (non-player living).
static bool gate_target_type(JNIEnv* env, const triggerbot::jni_cache& c, jobject target)
{
    const bool is_player = (c.player_class &&
        env->IsInstanceOf(target, c.player_class) == JNI_TRUE);

    switch (triggerbot::settings::target_type)
    {
    case 0: return true;            // All
    case 1: return is_player;       // Players
    case 2: return !is_player;      // Mobs (everything that isn't a player)
    default: return false;
    }
}

static bool gate_check_shield(JNIEnv* env, const triggerbot::jni_cache& c, jobject target)
{
    if (!triggerbot::settings::check_shield) return true;
    if (!c.living_entity_class || !c.living_entity_is_blocking_mid) return true;
    if (env->IsInstanceOf(target, c.living_entity_class) != JNI_TRUE) return true;

    jboolean blocking = env->CallBooleanMethod(target, c.living_entity_is_blocking_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    return blocking == JNI_FALSE;
}

// Friend check: kept as a stub — Vanta has no friends module, so every
// target is treated as "not a friend" and passes through.
static bool gate_not_friend(JNIEnv* /*env*/, jobject /*target*/) { return true; }

enum class TimingGate { Allow, Skip };

static TimingGate check_timing_gate(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject local_player)
{
    if (!cooldown_meets_threshold(env, c, local_player))
        return TimingGate::Skip;

    const uint64_t now = GetTickCount64();
    const uint64_t min_interval = holding_weapon(env, c, local_player) ? 450 : 200;
    if (g_last_attack_time_ms > 0 && now - g_last_attack_time_ms < min_interval)
        return TimingGate::Skip;

    return TimingGate::Allow;
}

static void post_attack_bookkeeping()
{
    const uint64_t now = GetTickCount64();
    g_last_attack_time_ms = now;
    g_last_combat_time_ms.store(now, std::memory_order_release);
    g_target_acquired_ms = now;
    g_dbg_attacks.fetch_add(1, std::memory_order_relaxed);
    g_dbg_last_attack_ms.store(g_last_attack_time_ms, std::memory_order_relaxed);
    g_pending_on_hit.fetch_add(1, std::memory_order_relaxed);
    g_pending_wtap_hit.fetch_add(1, std::memory_order_relaxed);
}

static bool gate_module_enabled()
{
    if (!triggerbot::settings::enabled) {
        TB_DBG("gate", "module_enabled=false → skip (settings::enabled is 0)");
        return false;
    }

    HWND mc_window = vanta::platform::findGameWindow();
    HWND fg        = GetForegroundWindow();
    if (!mc_window) {
        TB_DBG("gate", "findGameWindow() returned null — game window not resolved yet");
        return false;
    }
    if (fg != mc_window) {
        TB_DBG("gate", "game not foreground (mc_window=%p fg=%p)",
               (void*)mc_window, (void*)fg);
        return false;
    }
    TB_DBG("gate", "module_enabled=true (mc_window=%p is fg)", (void*)mc_window);
    return true;
}

// Re-validates, at the EXACT moment doAttack fires, that the crosshair is on a
// valid triggerbot target. Suprime un click sintético cuyo target se escapó
// entre la validación del world-tick y la fase de input.
static bool synthetic_target_still_valid(JNIEnv* env, const triggerbot::jni_cache& c,
    jobject minecraft, float reserved_target)
{
    jobject local_player = env->GetObjectField(minecraft, c.minecraft_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!local_player) return false;

    LocalRef player(env, local_player);
    if (!triggerbot::settings::enabled || (triggerbot::settings::show_gui && !triggerbot::settings::work_in_screen)) return false;
    if (!cooldown_meets_threshold(env, c, player.get(), reserved_target)) return false;
    if (check_hit_gate(env, c, player.get()) != HitGate::Allow) return false;

    jobject target_raw = resolve_crosshair_entity(env, c, minecraft);
    if (!target_raw)
    {
        // Fall back to the extended-reach raycast so the doAttack gate
        // matches the same target the world-tick side just picked.
        // Gate on reach > vanilla for the same reason as try_trigger_attack.
        const double reach = std::max(0.0, static_cast<double>(triggerbot::settings::attack_range));
        if (reach > 3.0)
            target_raw = raycast_extended_reach(env, c, minecraft, player.get(), reach);
    }
    LocalRef target(env, target_raw);
    if (!target) return false;
    {
        std::lock_guard<std::mutex> lock(g_visual_focus_mx);
        if (g_last_target && env->IsSameObject(target.get(), g_last_target) == JNI_FALSE) {
            if (!focus_allows_target(env, target.get())) return false;
        }
    }
    if (env->IsSameObject(target.get(), player.get()) == JNI_TRUE) return false;

    if (!gate_target_type(env, c, target.get()))   return false;
    if (!gate_check_shield(env, c, target.get()))   return false;
    if (!gate_not_friend(env, target.get()))        return false;
    if (!focus_allows_target(env, target.get())) return false;
    if (!target_passes_validation(env, c, player.get(), target.get())) return false;

    return true;
}

// hkDoAttack: gate ONLY the bot's synthetic click; pass manual clicks straight
// through. The synthetic click is matched within a time window, re-validated at
// fire time (anti air-swing), and book-kept on a clean call.
// Is the local player sprinting right now? Reads through MinecraftClient, so
// hkDoAttack can sample it either side of the original call. Failure reports
// false, which only ever means "no reset detected" — never a spurious one.
static bool read_player_sprinting(JNIEnv* env, jobject mc)
{
    const auto& c = triggerbot::get_cache();
    if (!c.minecraft_player_fid || !c.entity_is_sprinting_mid) return false;

    jobject player = env->GetObjectField(mc, c.minecraft_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!player) return false;

    const jboolean v = env->CallBooleanMethod(player, c.entity_is_sprinting_mid);
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    env->DeleteLocalRef(player);
    return ok && v == JNI_TRUE;
}

static void advance_attack_variation(JNIEnv* env, jobject minecraft) {
    ++g_consecutive_hits;
    const auto& cache = triggerbot::get_cache();
    LocalRef player(env, env->GetObjectField(minecraft, cache.minecraft_player_fid));
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (player) generate_next_offset(env, cache, player.get());
}

static float attack_charge(JNIEnv* env, jobject minecraft) {
    const auto& cache = triggerbot::get_cache();
    LocalRef player(env, env->GetObjectField(minecraft, cache.minecraft_player_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return player ? get_cooldown_progress(env, cache, player.get()) : 0;
}
static void confirm_recharge(float before, float after) {
    std::lock_guard lock(g_recharge_mx);
    g_recharge.after_attack(before, after, GetTickCount64());
}
static triggerbot::pending_click::Click consume_synthetic_click() {
    std::lock_guard lock(g_synthetic_click_mx);
    return g_synthetic_click.consume();
}
static jboolean hkDoAttackBody(JNIEnv* env, jobject thiz)
{
    if (!ORIG_doAttack || !g_minecraft_class) return JNI_FALSE;

    // Guard de shutdown: pasar el ataque al original sin más.
    if (vanta::modules::triggerbot::g_shutting_down.load(std::memory_order_acquire))
    {
        jboolean r = JNI_FALSE;
        __try {
            r = env->CallNonvirtualBooleanMethod(thiz, g_minecraft_class, ORIG_doAttack);
            if (env->ExceptionCheck()) env->ExceptionClear();
        } __except (EXCEPTION_EXECUTE_HANDLER) { r = JNI_FALSE; }
        return r;
    }

    // Anti Action: a sword swung into a raised shield only feeds the shield
    // its disable timer and costs the attacker the cooldown. Eaten whole --
    // no original, so no attack packet and no swing -- exactly as if the
    // click had not happened. Cheap and exception-safe; answers false the
    // moment the module is off.
    if (::vanta::modules::antiaction::should_block_attack(env, thiz)) {
        return JNI_FALSE;
    }

    // Silent aim owns the rotation but has not published a target yet — the
    // rotation is still swinging toward it. Any attack in this window is
    // indefensible to the server, so it does not go out at all.
    //
    // The case that actually bites is a BLOCK under the real crosshair: vanilla
    // doAttack answers that with attackBlock() -> START_DIGGING, and the dig is
    // then justified by an angle pointing at the enemy instead of at the block.
    // That is Grim's RotationBreak, and it is the half handleBlockBreaking's
    // suppression cannot reach, because that method only covers the HELD click
    // while this covers the first one.
    //
    // Once a target IS published the crosshair gets spoofed to it a few lines
    // below, so there is no block to dig and this never fires.
    // Gated on "the spoof is FAR", not merely "there is a spoof". While the
    // sent rotation is within a few degrees of the player's own view -- which
    // is most of the release, and the moments before the aim locks -- whatever
    // they clicked is also what the server raytraces onto, so the hit is
    // honest and there is no reason to eat it.
    //
    // And when it IS far, the click becomes a pure SWING rather than nothing.
    // What has to be withheld is the packet that names a target: an
    // InteractEntity the server cannot justify, or a START_DIGGING whose block
    // is not under the sent angle. The arm swing names nothing and is safe, so
    // eating it only made the client feel broken -- "it will not even let me
    // swing at air". Same two calls vanilla's own doAttack makes, in the same
    // order, then the original is skipped.
    if (!g_silentaim_active.load(std::memory_order_acquire)
        && ::vanta::modules::silentaim::attack_would_be_unjustified()) {
        const auto& c_sw = triggerbot::get_cache();
        if (c_sw.minecraft_player_fid) {
            jobject p = env->GetObjectField(thiz, c_sw.minecraft_player_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (p) {
                if (c_sw.player_reset_last_attacked_mid) {
                    env->CallVoidMethod(p, c_sw.player_reset_last_attacked_mid);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (c_sw.living_entity_swing_hand_mid && c_sw.hand_main_hand_ref) {
                    env->CallVoidMethod(p, c_sw.living_entity_swing_hand_mid,
                                        c_sw.hand_main_hand_ref);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                env->DeleteLocalRef(p);
            }
        }
        return JNI_FALSE;
    }

    triggerbot::pending_click::Click click;
    click = consume_synthetic_click();
    const bool ours = click.id != 0;
    const bool is_miss = click.miss;
    if (ours && GetTickCount64() - click.sent_ms > SYNTHETIC_CLICK_WINDOW_MS)
        return JNI_FALSE;
    if (ours)
    {
        const auto& c = triggerbot::get_cache();
        if (!cache_is_complete(c) || !synthetic_target_still_valid(env, c, thiz, click.cooldown_target))
            return JNI_FALSE;

        // ── Forced miss ──────────────────────────────────────────
        // The dice rolled a miss. Reproduce a genuine 1.9+ attack
        // swing at the air, but never let attackEntity run so no hit
        // registers.
        //
        // Two calls, in the same order vanilla doAttack uses:
        //   1. resetLastAttackedTicks() — resets the attack-strength
        //      cooldown so the first-person weapon plays its recharge
        //      dip. WITHOUT this the swing renders as the bare
        //      "1.8-style" / block-mining swing the user reported.
        //   2. swingHand(MAIN_HAND) — the arm-swing animation + the
        //      SWING_ARM packet.
        // We then return JNI_FALSE so the original doAttack (and its
        // attackEntity call) never runs → guaranteed whiff.
        if (is_miss)
        {
            if (c.minecraft_player_fid)
            {
                jobject player = env->GetObjectField(thiz, c.minecraft_player_fid);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (player)
                {
                    if (c.player_reset_last_attacked_mid)
                    {
                        env->CallVoidMethod(player, c.player_reset_last_attacked_mid);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (c.living_entity_swing_hand_mid && c.hand_main_hand_ref)
                    {
                        env->CallVoidMethod(player, c.living_entity_swing_hand_mid,
                                            c.hand_main_hand_ref);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    env->DeleteLocalRef(player);
                }
            }
            // Suppress the real attack — guaranteed miss.
            return JNI_FALSE;
        }

    }
    else if (triggerbot::settings::enabled)
    {
        // Manual player click while TriggerBot is enabled:
        // Gate hits on entities to strictly prevent weak 1.8 spam hits (<0.90F cooldown)
        // and prevent wasting hits on invulnerable targets (hurtTime > 0 = freehit).
        const auto& c_tb = triggerbot::get_cache();
        if (cache_is_complete(c_tb) && c_tb.minecraft_crosshair_target_fid && c_tb.entity_hit_result_class)
        {
            jobject cur = env->GetObjectField(thiz, c_tb.minecraft_crosshair_target_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (cur)
            {
                const bool is_entity = env->IsInstanceOf(cur, c_tb.entity_hit_result_class) == JNI_TRUE;
                if (is_entity && c_tb.minecraft_player_fid)
                {
                    jobject lp = env->GetObjectField(thiz, c_tb.minecraft_player_fid);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (lp)
                    {
                        const float prog = get_cooldown_progress(env, c_tb, lp, 0.5f);
                        const float min_prog = 0.90f;
                        if (prog < min_prog)
                        {
                            // Cooldown not charged! Suppress attackEntity so the player
                            // doesn't deal a weak 1.8 spam hit and doesn't wipe their cooldown bar.
                            env->DeleteLocalRef(lp);
                            env->DeleteLocalRef(cur);
                            return JNI_FALSE;
                        }

                        if (c_tb.entity_hit_result_get_entity_mid)
                        {
                            jobject target_ent = env->CallObjectMethod(cur, c_tb.entity_hit_result_get_entity_mid);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (target_ent)
                            {
                                if (target_in_invuln(env, c_tb, target_ent))
                                {
                                    // Target still in damage immunity! Suppress freehit.
                                    env->DeleteLocalRef(target_ent);
                                    env->DeleteLocalRef(lp);
                                    env->DeleteLocalRef(cur);
                                    return JNI_FALSE;
                                }
                                env->DeleteLocalRef(target_ent);
                            }
                        }
                        env->DeleteLocalRef(lp);
                    }
                }
                env->DeleteLocalRef(cur);
            }
        }
    }

    // v124: silent aim spoof. PEEK (no consume) del target publicado. Si
    // silent aim tiene target activo, spoofear MC.crosshairTarget al target
    // antes de llamar al doAttack original → hit vanilla al enemigo del
    // silent aim con animación 100% natural. Después del call, restaurar el
    // crosshair original.
    // Nota: NO hacemos DeleteGlobalRef del sa_target — silent aim posee el
    // lifecycle via set_silentaim_target (borra el viejo al reemplazar).
    // Peek con load garantiza que múltiples clicks en el mismo tick usan
    // el mismo target (evita fallback a real crosshair en 2do click rápido).
    jobject saved_crosshair = nullptr;
    jobject sa_ehr = nullptr;
    jobject sa_target = g_silentaim_spoof_target.load(std::memory_order_acquire);
    const auto& c_sa = triggerbot::get_cache();
    if (sa_target && c_sa.minecraft_crosshair_target_fid &&
        c_sa.entity_hit_result_class && c_sa.entity_hit_result_ctor) {
        saved_crosshair = env->GetObjectField(thiz, c_sa.minecraft_crosshair_target_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); saved_crosshair = nullptr; }
        sa_ehr = env->NewObject(c_sa.entity_hit_result_class,
                                c_sa.entity_hit_result_ctor, sa_target);
        if (env->ExceptionCheck()) { env->ExceptionClear(); sa_ehr = nullptr; }
        if (sa_ehr) env->SetObjectField(thiz, c_sa.minecraft_crosshair_target_fid, sa_ehr);
    }

    // Extended-reach spoof: if this is our synthetic click, silent aim
    // isn't active, and vanilla's crosshairTarget doesn't hold an entity,
    // fall back to our own raycast. If it finds one within the user's
    // configured reach we wrap it in an EntityHitResult and swap it in
    // for the duration of the original doAttack() call — same mechanic
    // silent aim uses. Restore after.
    jobject ext_saved = nullptr;
    jobject ext_ehr   = nullptr;
    jobject ext_target = nullptr; // LOCAL ref we own — freed at end of scope
    if (ours && !sa_target && c_sa.minecraft_crosshair_target_fid &&
        c_sa.entity_hit_result_class && c_sa.entity_hit_result_ctor &&
        c_sa.minecraft_player_fid)
    {
        jobject cur = env->GetObjectField(thiz, c_sa.minecraft_crosshair_target_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();

        const bool vanilla_has_entity = (cur &&
            env->IsInstanceOf(cur, c_sa.entity_hit_result_class) == JNI_TRUE);

        if (!vanilla_has_entity)
        {
            const double reach = std::max(0.0,
                static_cast<double>(triggerbot::settings::attack_range));
            jobject lp = (reach > 3.0)
                ? env->GetObjectField(thiz, c_sa.minecraft_player_fid)
                : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); lp = nullptr; }
            if (lp)
            {
                ext_target = raycast_extended_reach(env, c_sa, thiz, lp, reach);
                env->DeleteLocalRef(lp);
            }
            if (ext_target)
            {
                ext_ehr = env->NewObject(c_sa.entity_hit_result_class,
                                         c_sa.entity_hit_result_ctor, ext_target);
                if (env->ExceptionCheck()) { env->ExceptionClear(); ext_ehr = nullptr; }
                if (ext_ehr) {
                    // Take ownership of `cur` (may legitimately be null when
                    // vanilla was aiming at pure air) so the restore path can
                    // put it back and free it.
                    ext_saved = cur;
                    env->SetObjectField(thiz, c_sa.minecraft_crosshair_target_fid, ext_ehr);
                } else if (cur) {
                    // Spoof failed — free the local ref, leave field as-is.
                    env->DeleteLocalRef(cur);
                }
            }
            else if (cur)
            {
                env->DeleteLocalRef(cur);
            }
        }
        else if (cur)
        {
            env->DeleteLocalRef(cur);
        }
    }

    // Sprint reset — read across the original call, because the ONLY reliable
    // way to know whether this attack reset the sprint is to watch vanilla
    // decide. PlayerEntity.attack() clears it on a strong hit and leaves it
    // alone on a weak one, and reproducing that condition here would mean
    // duplicating the attack-cooldown maths and getting it wrong the day it
    // changes. Observing the flag costs two calls and cannot disagree.
    const bool was_sprinting = read_player_sprinting(env, thiz);

    // If this attack is from triggerbot (ours), the player is in the air jumping (!grounded),
    // and wait_for_crit is enabled:
    // In vanilla Minecraft, "bl3 = bl3 && !this.isSprinting()". If the player is sprinting,
    // vanilla cancels the critical hit and turns it into a normal sprint knockback hit!
    // By resetting sprint right here before ORIG_doAttack, vanilla evaluates !isSprinting()
    // as TRUE, turning the attack into a 100% guaranteed CRITICAL HIT instead of a normal hit!
    bool dropped_sprint_for_crit = false;
    if (ours && triggerbot::settings::wait_for_crit)
    {
        const auto& c_tb = triggerbot::get_cache();
        if (c_tb.minecraft_player_fid && c_tb.entity_is_on_ground_mid && c_tb.entity_set_sprinting_mid)
        {
            jobject lp = env->GetObjectField(thiz, c_tb.minecraft_player_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (lp)
            {
                jboolean og = env->CallBooleanMethod(lp, c_tb.entity_is_on_ground_mid);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (og == JNI_FALSE && was_sprinting)
                {
                    env->CallVoidMethod(lp, c_tb.entity_set_sprinting_mid, JNI_FALSE);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    dropped_sprint_for_crit = true;
                }
                env->DeleteLocalRef(lp);
            }
        }
    }

    const float chargeBefore = ours ? attack_charge(env, thiz) : 0;
    jboolean r = env->CallNonvirtualBooleanMethod(thiz, g_minecraft_class, ORIG_doAttack);
    if (ours && !env->ExceptionCheck()) confirm_recharge(chargeBefore, attack_charge(env, thiz));
    if (env->ExceptionCheck()) { env->ExceptionClear(); }

    // Vanilla just reset the sprint. Release the binding NOW: this runs inside
    // handleInputEvents, so tickMovement has not re-read the key yet, and a
    // module holding it down would otherwise turn the sprint back on within
    // this same tick -- leaving sendMovementPackets with nothing to report
    // while the player's velocity had already been cut to 0.6. See
    // request_sprint_reset() for why that is what Simulation sees.
    if ((was_sprinting || dropped_sprint_for_crit) && !read_player_sprinting(env, thiz))
        ::vanta::shared::request_sprint_reset(env, thiz);

    // Restore crosshair (silent aim NOT owner of sa_target lifecycle here).
    if (sa_target) {
        if (sa_ehr) {
            env->SetObjectField(thiz, c_sa.minecraft_crosshair_target_fid, saved_crosshair);
            env->DeleteLocalRef(sa_ehr);
        }
        if (saved_crosshair) env->DeleteLocalRef(saved_crosshair);
    }

    // Restore extended-reach spoof and drop the transient EntityHitResult
    // + entity ref we introduced. ext_saved may legitimately be null (the
    // original crosshair was air/block), we still restore that.
    if (ext_ehr) {
        env->SetObjectField(thiz, c_sa.minecraft_crosshair_target_fid, ext_saved);
        env->DeleteLocalRef(ext_ehr);
    }
    if (ext_saved) env->DeleteLocalRef(ext_saved);
    if (ext_target) env->DeleteLocalRef(ext_target);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return r; }
    // doAttack() returns false for ENTITY hits (the bool reports block-breaking),
    // so we book-keep on any clean call, not just r == JNI_TRUE.
    post_attack_bookkeeping();
    if (ours) advance_attack_variation(env, thiz);
    return r;
}

static jboolean JNICALL hkDoAttack(JNIEnv* env, jobject thiz) {
    struct AttackScope {
        AttackScope() {
            std::lock_guard lock(g_synthetic_click_mx);
            g_synthetic_click.begin_attack();
        }
        ~AttackScope() {
            std::lock_guard lock(g_synthetic_click_mx);
            g_synthetic_click.end_attack();
        }
    } scope;
    return hkDoAttackBody(env, thiz);
}
// v124b: hkHandleBlockBreaking — MinecraftClient.handleBlockBreaking(Z)V.
// Se llama cada frame que el LMB está apretado. Su función normal es leer
// crosshairTarget REAL y, si es un bloque, enviar START_DIGGING (primera
// vez) o progress packets (frames siguientes) via ClientPlayerInteraction
// Manager.updateBlockBreakingProgress.
//
// Cuando silent aim tiene target activo, hacemos NO-OP: retornamos sin
// llamar al original. Esto suprime:
//   - START_DIGGING packet → Grim RotationBreak no dispara (no hay dig
//     packet con rotación mismatched a spoofed yaw).
//   - Doble packet en el mismo tick (InteractEntity + START_DIGGING) →
//     Grim MultiActionsF no dispara.
//
// El doAttack sigue funcionando normal: hkDoAttack spoofea crosshair a
// entity → MC sends InteractEntity → hit vanilla. Simplemente eliminamos
// el path paralelo de "también intentar minar el bloque".
// Freecam stand-down. While the camera is detached the crosshair target and
// the isCamera semantics belong to the camera entity and the player's rotation
// is frozen on purpose: nothing here may evaluate a target, press a key or
// start digging. Its own function because the detours below hold __try
// blocks, which cannot share a scope with the std::string temporary this
// lookup builds.
static bool freecam_active()
{
    return ::vanta::vgui::bridge::isEnabled("Freecam");
}

static void JNICALL hkHandleBlockBreaking(JNIEnv* env, jobject thiz, jboolean breaking)
{
    // Pre-tick anchor for silent aim's movement correction.
    //
    // This detour runs inside MinecraftClient.handleInputEvents, i.e. once per
    // tick and BEFORE world.tickEntities() drives the player — the only phase
    // where the spoofed rotation can be put in place in time for travel().
    // Silent aim borrows it rather than installing its own hook there because
    // handleInputEvents itself is a dispatcher: hooking it makes the original
    // throw on its internal this.doAttack() call and attacks stop working.
    // This method is private AND a leaf, which is what makes it detour cleanly.
    //
    // Called before the early-out below on purpose: the suppression path is
    // exactly the case where silent aim HAS a target and most needs the frame.
    ::vanta::modules::silentaim::on_pre_player_tick(env, thiz);

    // Freecam: vanilla path only (the attack key never reaches the game while
    // the capture is up, so `breaking` is false and the original just resets).
    if (freecam_active()) {
        if (ORIG_handleBlockBreaking && g_minecraft_class) {
            env->CallNonvirtualVoidMethod(thiz, g_minecraft_class, ORIG_handleBlockBreaking, breaking);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        return;
    }

    // Suprimir el block breaking mientras silent aim tenga la rotación tomada.
    //
    // La condición es "hay una rotación spoofeada en el cable", NO "hay target
    // publicado". Son cosas distintas y confundirlas costó una tormenta de
    // RotationBreak: el target solo se publica una vez que la rotación alcanzó
    // al objetivo, así que durante el giro g_silentaim_active queda en false —
    // y ahí vanilla mandaba START_DIGGING contra el bloque del crosshair REAL
    // mientras el ángulo que viajaba apuntaba al enemigo. Ese desajuste es
    // exactamente lo que mide RotationBreak.
    //
    // NO llamamos ORIG_handleBlockBreaking → nada de packets START_DIGGING /
    // block-break-progress → nada de flags.
    if (g_silentaim_active.load(std::memory_order_acquire)
        || ::vanta::modules::silentaim::rotation_spoof_active()) {
        return;
    }

    // Path normal: llamar al original.
    if (ORIG_handleBlockBreaking && g_minecraft_class) {
        env->CallNonvirtualVoidMethod(thiz, g_minecraft_class, ORIG_handleBlockBreaking, breaking);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

// v124: silent aim publica su target para spoof en doAttack. Convertimos a
// global ref y almacenamos. El ref persiste hasta el próximo set_silentaim_
// target (silent aim republica cada tick con target valido, publica nullptr
// al perder target o desactivarse). hkDoAttack hace peek (no consume) →
// múltiples clicks en un tick usan el mismo target. hkHandleBlockBreaking
// consulta g_silentaim_active para suprimir el START_DIGGING que dispararia
// RotationBreak/MultiActionsF en Grim.
// Llamada desde el GAME THREAD (silent aim tick_dispatch).
void set_silentaim_target(JNIEnv* env, jobject target)
{
    if (!env) return;
    // Reemplazo atómico del ref: exchange nos da el viejo para borrarlo sin
    // ventana en la que ambos existan. hkDoAttack en el mismo thread no puede
    // estar leyéndolo concurrentemente (game thread es single).
    jobject old = g_silentaim_spoof_target.exchange(nullptr, std::memory_order_acq_rel);
    if (old) env->DeleteGlobalRef(old);
    if (target) {
        jobject gref = env->NewGlobalRef(target);
        g_silentaim_spoof_target.store(gref, std::memory_order_release);
        g_silentaim_active.store(true, std::memory_order_release);
    } else {
        g_silentaim_active.store(false, std::memory_order_release);
    }
}

static void run_original_tick(JNIEnv* env, jobject thiz)
{
    if (ORIG_tickMovement && g_clientplayer_class)
    {
        env->CallNonvirtualVoidMethod(thiz, g_clientplayer_class, ORIG_tickMovement);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

// Injects a real left-click. The game's own un-hooked handleInputEvents fires
// doAttack() at its natural client-tick phase → genuine vanilla swing.
static bool send_attack_click(float target_progress, bool miss = false)
{
    if (!ORIG_doAttack || !g_minecraft_class) return false;
    std::uint64_t id = 0;
    {
        std::lock_guard lock(g_synthetic_click_mx);
        if (g_synthetic_click.pending(GetTickCount64(), SYNTHETIC_CLICK_WINDOW_MS)) return false;
        id = g_synthetic_click.reserve(GetTickCount64(), target_progress, miss);
    }
    if (!id) return false;
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, input, sizeof(INPUT));
    if (sent == 0) {
        std::lock_guard lock(g_synthetic_click_mx);
        g_synthetic_click.cancel(id);
        return false;
    }
    if (sent == 1) SendInput(1, &input[1], sizeof(INPUT));
    g_target_acquired_ms = GetTickCount64();
    return true;
}
// Injects a left-click flagged as a MISS. Same SendInput path as a real
// attack so the timing/threading is identical, but hkDoAttack will turn
// it into a pure swingHand (animation only) and suppress the actual
// attack → guaranteed whiff with a clean vanilla swing.
// Tracks the local player's view rotation between fire opportunities so
// we can detect sharp flicks. Misses are only allowed while the player
// is actively swinging their aim (a "giro brusco") — that's when a human
// realistically whiffs. During steady tracking the bot always hits.
static float    g_prev_yaw      = 0.0f;
static float    g_prev_pitch    = 0.0f;
static uint64_t g_prev_rot_ms   = 0;
static bool     g_rot_init      = false;

static void try_trigger_attack(JNIEnv* env, jobject local_player)
{
    TB_DBG("try", "entered try_trigger_attack (player=%p)", (void*)local_player);

    if (!gate_module_enabled())
    {
        g_dbg_skipped_disabled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    triggerbot::init_cache(env);
    const auto& c = triggerbot::get_cache();
    if (!cache_is_complete(c))
    {
        TB_DBG("try", "cache NOT complete — JNI mappings failed; check earlier init_cache log");
        g_dbg_skipped_disabled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t now_ms = GetTickCount64();
    const bool moving = (GetAsyncKeyState('W') & 0x8000) != 0 ||
                        (GetAsyncKeyState('A') & 0x8000) != 0 ||
                        (GetAsyncKeyState('S') & 0x8000) != 0 ||
                        (GetAsyncKeyState('D') & 0x8000) != 0 ||
                        (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 ||
                        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (moving) {
        g_last_moving_time_ms.store(now_ms, std::memory_order_release);
    }
    if (c.living_entity_hurt_time_fid && local_player) {
        jint ht = env->GetIntField(local_player, c.living_entity_hurt_time_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (ht > 0) {
            g_last_combat_time_ms.store(now_ms, std::memory_order_release);
        }
    }

    LocalRef minecraft(env, resolve_minecraft(env, c));
    if (!minecraft)
    {
        TB_DBG("try", "resolve_minecraft returned null");
        g_dbg_skipped_disabled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!triggerbot::settings::focus_mode && !triggerbot::settings::only_on_hit) {
        clear_focused_entity(env);
        clear_visual_focus(env);
    } else {
        LocalRef world(env, c.minecraft_world_fid ? env->GetObjectField(minecraft.get(), c.minecraft_world_fid) : nullptr);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        LocalRef focused(env, borrow_focused_entity(env));
        if (focused) {
            bool sameWorld;
            {
                std::lock_guard lock(g_visual_focus_mx);
                sameWorld = world && g_focused_world && env->IsSameObject(world.get(), g_focused_world) == JNI_TRUE;
            }
            const uint64_t lastMove = g_last_moving_time_ms.load(std::memory_order_acquire);
            const bool standStill20s = (lastMove > 0 && (now_ms - lastMove) >= 20000);

            if (!sameWorld || target_is_dead(env, c, focused.get()) || standStill20s) {
                clear_focused_entity(env, focused.get());
                clear_visual_focus(env);
            }
        }
    }
    if (!gate_attack_in_gui(env, c, minecraft.get()))
    {
        TB_DBG("try", "gate_attack_in_gui blocked (GUI open, WorkInScreens=off)");
        g_dbg_skipped_disabled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    UseGate ug = gate_when_use_hand(env, c, local_player);
    if (ug == UseGate::SkipUsing)
    {
        TB_DBG("try", "gate_when_use_hand=SkipUsing (right-click held)");
        g_dbg_skipped_using_item.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ug == UseGate::SkipPostUse)
    {
        TB_DBG("try", "gate_when_use_hand=SkipPostUse (post-use refractory)");
        g_dbg_skipped_post_use.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Prefer the game's own crosshair pick (0..~3 blocks, respects wall
    // occlusion for free). Fall back to the extended-reach entity raycast
    // whenever the built-in pick came back empty / non-entity — that path
    // is what unlocks the range slider above vanilla reach.
    jobject target_raw = nullptr;
    {
        LocalRef hit_result(env, env->GetObjectField(minecraft.get(), c.minecraft_crosshair_target_fid));
        if (hit_result &&
            env->IsInstanceOf(hit_result.get(), c.entity_hit_result_class) == JNI_TRUE)
        {
            target_raw = env->CallObjectMethod(hit_result.get(),
                c.entity_hit_result_get_entity_mid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); target_raw = nullptr; }
        }
    }

    if (!target_raw)
    {
        const double reach = std::max(0.0, static_cast<double>(triggerbot::settings::attack_range));
        // Only scan past vanilla when the user actually asked for it; below
        // 3.0 the game's own occlusion-aware pick is authoritative and we
        // should not second-guess it (would attack targets through walls).
        if (reach > 3.0)
        {
            target_raw = raycast_extended_reach(env, c, minecraft.get(), local_player, reach);
            if (target_raw)
                TB_DBG("try", "extended-reach raycast hit entity=%p", (void*)target_raw);
        }
    }

    LocalRef target(env, target_raw);

    if (!target)
    {
        g_target_acquired_ms = 0;
        TB_DBG("try", "no crosshair target and extended raycast found nothing");
        g_dbg_skipped_no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (env->IsSameObject(target.get(), local_player) == JNI_TRUE)
    {
        g_target_acquired_ms = 0;
        TB_DBG("try", "target IS local player (self)");
        g_dbg_skipped_no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TB_DBG("try", "have target entity=%p", (void*)target.get());

    // Never attack something invisible. Gladiator's HoneypotCheck spawns a
    // client-side-only fake player one block behind you, flagged invisible, and
    // reports KillAura the moment anything hits it. Both routes above are
    // exposed: the extended-reach raycast seeks entities on its own, and even
    // the vanilla crosshair pick will happily return bait that intercepts the
    // ray at one block once the player turns round.
    //
    // Filtering here rather than in each route covers both, and it costs a
    // capability that was never defensible anyway: a human cannot aim at what
    // they cannot see.
    if (c.entity_is_invisible_mid)
    {
        const jboolean invis = env->CallBooleanMethod(target.get(), c.entity_is_invisible_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (invis == JNI_TRUE)
        {
            g_target_acquired_ms = 0;
            TB_DBG("try", "target is invisible — refusing (honeypot bait)");
            g_dbg_skipped_no_target.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    if (target_is_dead(env, c, target.get()))
    {
        g_target_acquired_ms = 0;
        TB_DBG("try", "target is dead or spectator — refusing");
        g_dbg_skipped_no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!gate_target_type(env, c, target.get()))
    {
        TB_DBG("try", "gate_target_type blocked (target_type=%d)",
               static_cast<int>(triggerbot::settings::target_type));
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (target_in_invuln(env, c, target.get()))
    {
        TB_DBG("try", "target in invuln (hurtTime > %d) — waiting", (int)triggerbot::settings::max_hurt_time);
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!gate_check_shield(env, c, target.get()))
    {
        TB_DBG("try", "gate_check_shield blocked (target blocking)");
        g_dbg_skipped_shield.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (::vanta::social::isFriend(env, target.get()))
    {
        TB_DBG("try", "target is a friend — skipping");
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (triggerbot::settings::check_teams && ::vanta::social::isTeammate(env, local_player, target.get()))
    {
        TB_DBG("try", "target is a teammate — skipping");
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (synthetic_click_pending()) return;

    // ── Target change detection: reset streak counter and track acquisition time ──
    {
        std::lock_guard<std::mutex> lock(g_visual_focus_mx);
        if (g_last_target && env->IsSameObject(target.get(), g_last_target) == JNI_FALSE)
        {
            g_consecutive_hits = 0;
            if (env) env->DeleteGlobalRef(g_last_target);
            g_last_target = env->NewGlobalRef(target.get());
            g_target_acquired_ms = now_ms;
        }
        else if (!g_last_target)
        {
            g_last_target = env->NewGlobalRef(target.get());
            g_target_acquired_ms = now_ms;
        }
    }

    if (!focus_allows_target(env, target.get())) {
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (check_timing_gate(env, c, local_player) == TimingGate::Skip)
    {
        TB_DBG("try", "check_timing_gate=Skip (attack cooldown not charged / min interval)");
        g_dbg_skipped_cooldown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (check_hit_gate(env, c, local_player) == HitGate::Skip)
    {
        TB_DBG("try", "check_hit_gate=Skip (crit window / require-sprint)");
        g_dbg_skipped_crit.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!target_passes_validation(env, c, local_player, target.get()))
    {
        TB_DBG("try", "target_passes_validation=false (invuln / dead / out of reach / no weapon)");
        g_dbg_skipped_validation.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TB_DBG("try", "ALL GATES PASSED — about to dispatch SendInput");

    // Slot restriction: if user has restricted firing to specific
    // hotbar slots, skip when the current selected slot isn't in the
    // mask. Silent abort (no counter bumped).
    if (slot_restricted(env, c, local_player))
        return;

    // Dispatch via a REAL synthetic left-click. Skip if one is still pending so
    // we don't queue duplicates; clear a stale flag so the bot never gets stuck.
    if (synthetic_click_pending()) return;

    // ── Miss chance ──────────────────────────────────────────────
    // Misses trigger in the two situations a human realistically
    // whiffs a target their crosshair is on:
    //   • FLICK — you swing your aim fast (sharp turn). The faster the
    //     flick, the higher the miss weight.
    //   • REACH — the target is near or past the edge of your reach
    //     (running away / spacing you out). The closer to max reach,
    //     the higher the miss weight.
    // While you track a player steadily AND they're comfortably in
    // range, the bot always hits.
    //
    // The effective chance = slider × max(flick_factor, range_factor)
    // × DAMPEN. We take the MAX so either a flick OR the target
    // escaping is enough to cause a whiff. A short refractory keeps
    // the fire cadence steady whether a swing hits or misses.
    {
        uint64_t now = GetTickCount64();

        // ── Sample rotation speed (deg/sec) ──────────────────────
        // enhance's sdk::entity_client wrapper is replaced by direct
        // field reads through Vanta's canonical mapping registry — same
        // two intermediary field IDs (Entity.yaw / Entity.pitch), no
        // wrapper allocation per frame.
        double rot_speed = 0.0;
        {
            float yaw = 0.f, pitch = 0.f;
            if (jfieldID yawF = ::vanta::jvm::fieldCanon(env, "Entity", "yaw")) {
                yaw = env->GetFloatField(local_player, yawF);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (jfieldID pitchF = ::vanta::jvm::fieldCanon(env, "Entity", "pitch")) {
                pitch = env->GetFloatField(local_player, pitchF);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            if (g_rot_init) {
                uint64_t dt_ms = now - g_prev_rot_ms;
                if (dt_ms > 0 && dt_ms < 250) {
                    float dyaw = yaw - g_prev_yaw;
                    while (dyaw > 180.f)  dyaw -= 360.f;
                    while (dyaw < -180.f) dyaw += 360.f;
                    float dpitch = pitch - g_prev_pitch;
                    double moved = std::sqrt((double)dyaw * dyaw +
                                             (double)dpitch * dpitch);
                    rot_speed = moved / ((double)dt_ms / 1000.0);
                }
            }
            g_prev_yaw   = yaw;
            g_prev_pitch = pitch;
            g_prev_rot_ms = now;
            g_rot_init   = true;
        }

        if (now < g_miss_until_ms) return;

        int chance = triggerbot::settings::miss_chance;
        if (chance < 0)  chance = 0;
        if (chance > 75) chance = 75;

        if (chance > 0) {
            // Flick factor: 0 below 150 deg/s (steady), ramps to 1 at
            // 480 deg/s (a clear sharp turn). Lower threshold than
            // Flick factor. Threshold raised so ordinary tracking
            // (following a strafing target, small corrections) does
            // NOT count as a flick. Only a deliberate fast turn —
            // 300°/s and up — starts to weigh in, reaching full weight
            // at 620°/s. This stops the "misses while I'm calmly aimed
            // at the player" false whiffs.
            const double flick_lo = 300.0;
            const double flick_hi = 620.0;
            double flick_factor = 0.0;
            if (rot_speed > flick_lo) {
                flick_factor = (rot_speed - flick_lo) / (flick_hi - flick_lo);
                if (flick_factor > 1.0) flick_factor = 1.0;
            }

            // Range factor. Clean zone pushed out to 88% of reach, so
            // any comfortable hittable distance NEVER whiffs — only the
            // last sliver of reach (target physically spacing you out)
            // can miss. At range 2.999 that's ~2.64 blocks clean, miss
            // only past that toward the 3.0 edge.
            double dist  = eye_to_target_distance(env, c, local_player, target.get());
            double range = (double)triggerbot::settings::attack_range;
            double clean_zone = range * 0.88;
            double range_factor = 0.0;
            if (dist > clean_zone && range > clean_zone) {
                range_factor = (dist - clean_zone) / (range - clean_zone);
                if (range_factor > 1.0) range_factor = 1.0;
            }

            // Either condition can cause a whiff → take the max. With
            // the stricter gates above, a steady aim at a hittable
            // distance gives combined = 0 → guaranteed hit.
            double combined = (flick_factor > range_factor) ? flick_factor : range_factor;

            // Light dampening only.
            const double DAMPEN = 0.90;
            double effective = (double)chance * combined * DAMPEN;

            if (effective > 0.0) {
                double roll = (double)(g_rng() % 100000) / 1000.0;
                if (roll < effective) {
                    g_miss_until_ms = now + 450 + (unsigned)(g_rng() % 200);
                    // Guaranteed whiff with a clean swing animation.
                    // Don't bump g_consecutive_hits — wasn't a hit.
                    const bool grounded = player_on_ground(env, c, local_player);
                    double vy = 0.0;
                    if (!grounded) read_velocity_y(env, c, local_player, vy);
                    send_attack_click(effective_cooldown_target(grounded, vy), true);
                    return;
                }
            }
        }
    }

    const bool grounded = player_on_ground(env, c, local_player);
    double vy = 0.0;
    if (!grounded) read_velocity_y(env, c, local_player, vy);
    send_attack_click(effective_cooldown_target(grounded, vy));
}

static void JNICALL hkTickMovement(JNIEnv* env, jobject thiz)
{
    // Guard de shutdown: pasar al original sin tocar el cache.
    if (vanta::modules::triggerbot::g_shutting_down.load(std::memory_order_acquire))
    {
        if (ORIG_tickMovement && g_clientplayer_class) {
            __try {
                env->CallNonvirtualVoidMethod(thiz, g_clientplayer_class, ORIG_tickMovement);
                if (env->ExceptionCheck()) env->ExceptionClear();
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return;
    }

    // Freecam: the tick itself must run (or the player freezes), but no
    // target is evaluated and nothing is pressed. See freecam_active.
    if (freecam_active())
    {
        if (ORIG_tickMovement && g_clientplayer_class) {
            env->CallNonvirtualVoidMethod(thiz, g_clientplayer_class, ORIG_tickMovement);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        return;
    }

    g_dbg_hook_fires.fetch_add(1, std::memory_order_relaxed);

    // Always sample the player's vertical state at the world-tick rate
    // (20 Hz) and stash it. The Prioritize-crits gate reads ONLY this
    // snapshot — never the live render-rate velocity — because the
    // server evaluates incoming attacks against the velocity from this
    // very tick. Reading live velocity from the render thread fires
    // crits on a state the server hasn't acknowledged yet, which lands
    // as a normal hit. This must run regardless of whether the render
    // thread is also driving dispatch.
    {
        triggerbot::init_cache(env);
        const auto& c = triggerbot::get_cache();
        if (cache_is_complete(c))
        {
            int new_state = TVS_GROUND;
            if (player_on_ground(env, c, thiz))
            {
                new_state = TVS_GROUND;
            }
            else
            {
                double vy = 0.0;
                if (read_velocity_y(env, c, thiz, vy))
                {
                    if (vy < -1e-4)      new_state = TVS_DESCENDING;
                    else if (vy > 1e-4)  new_state = TVS_ASCENDING;
                    else                 new_state = TVS_GROUND; // apex stationary frame
                }
            }
            g_tick_vert_state.store(new_state, std::memory_order_release);
        }
    }

    // If the render-thread dispatcher is alive and ticked recently, skip
    // the world-tick dispatch — render runs at 60-360 Hz and gives us
    // sub-tick reaction time. The world tick still calls run_original_tick
    // below so vanilla movement isn't disturbed.
    unsigned long long now    = GetTickCount64();
    unsigned long long render = g_last_render_dispatch_ms.load(std::memory_order_acquire);
    bool render_alive = (render != 0) && (now - render <= RENDER_HEARTBEAT_MS);

    if (!render_alive)
    {
        try_trigger_attack(env, thiz);
    }

    run_original_tick(env, thiz);
}

// Called from the render hook (wglSwapBuffers) — runs at 60-360 Hz instead
// of the 20 Hz world tick, dropping the validation latency from up to 50 ms
// down to ~3-16 ms. The synthetic click + doAttack hook still keep the
// vanilla swing animation untouched. Resolves the local player from
// MinecraftClient and forwards to try_trigger_attack.
//
// Safe to call from any thread that has a JNIEnv attached. Cheap when
// disabled (gate_module_enabled fast-path returns immediately).
void tick_from_render(JNIEnv* env)
{
    TB_DBG("render", "tick_from_render entered (env=%p)", (void*)env);
    if (!env) return;

    triggerbot::init_cache(env);
    const auto& c = triggerbot::get_cache();
    if (!cache_is_complete(c) || !c.minecraft_player_fid) {
        TB_DBG("render", "cache incomplete — mc=%p player_fid=%p ehr=%p attack_mid=%p hand=%p",
               (void*)c.minecraft_class, (void*)c.minecraft_player_fid,
               (void*)c.entity_hit_result_class,
               (void*)c.interaction_manager_attack_entity_mid,
               (void*)c.hand_class);
        return;
    }

    LocalRef minecraft(env, resolve_minecraft(env, c));
    if (!minecraft) {
        TB_DBG("render", "resolve_minecraft returned null");
        return;
    }


    jobject local_player = env->GetObjectField(minecraft.get(), c.minecraft_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); TB_DBG("render", "GetObjectField(player) threw"); return; }
    if (!local_player) { TB_DBG("render", "MinecraftClient.player == null (not in world)"); return; }

    LocalRef player(env, local_player);

    // Sample vertical state here too, at render rate. The proper place
    // for this is tick_from_movement_packets (world-tick aligned = same
    // vy the server sees), but Vanta has no sendMovementPackets JNIHook
    // to piggyback on. Sampling at render rate loses a tick of server-
    // alignment but the gravity-shifted threshold below still keeps
    // Priorize Crits honest: the state only flips to DESCENDING once
    // the client is a full tick past the apex, at which point the
    // server's next tick also sees vy < 0.
    {
        int new_state = TVS_GROUND;
        if (!player_on_ground(env, c, player.get())) {
            double vy = 0.0;
            if (read_velocity_y(env, c, player.get(), vy)) {
                new_state = (vy <= DESCENT_VELOCITY_THRESHOLD)
                          ? TVS_DESCENDING : TVS_ASCENDING;
            } else {
                new_state = TVS_ASCENDING;   // cannot read → hold, never guess
            }
        }
        g_tick_vert_state.store(new_state, std::memory_order_release);
    }

    g_last_render_dispatch_ms.store(GetTickCount64(), std::memory_order_release);

    // Freecam: the vertical-state sample above is kept honest, but no target
    // is evaluated and nothing is clicked. See freecam_active.
    if (freecam_active()) return;

    try_trigger_attack(env, player.get());
}

// Called from the GAME THREAD by throwpot::hooked_sendMovementPackets()
// at world-tick rate (20 Hz). Replaces the old hkTickMovement callback
// (class_746.method_6007 JNIHook) which was removed because under MC
// 1.21.7+ + JVM 21 the bytecode retransform generated a broken class
// copy that froze player movement.
//
// Same two responsibilities as the old hook:
//   1. Server-tick-aligned snapshot of the player's vertical state for
//      the "Priorize Crits" gate. This MUST run every world tick, even
//      when the render dispatcher is alive, because Priorize Crits
//      reads ONLY this snapshot — render-rate velocity reads would
//      decide based on a velocity the server hasn't acknowledged yet.
//   2. World-tick-rate dispatch fallback when the render dispatcher
//      went silent (Vulkan host, OpenGL hook lost, frozen frame, …).
//
// `local_player` is the `thiz` of sendMovementPackets, which is the
// ClientPlayerEntity instance — same pointer hkTickMovement received.
void tick_from_movement_packets(JNIEnv* env, jobject local_player)
{
    if (!env || !local_player) return;

    g_dbg_hook_fires.fetch_add(1, std::memory_order_relaxed);

    triggerbot::init_cache(env);
    const auto& c = triggerbot::get_cache();
    if (!cache_is_complete(c)) return;

    // ── Step 1: vertical-state snapshot for Priorize Crits ───────────
    // Sample point: this fires at the END of the tick (after tick-
    // Movement applied gravity). The server's view of the player's
    // vy lags ours by exactly one tick of gravity. We compensate by
    // shifting the DESCENDING threshold by GRAVITY_PER_TICK so the
    // gate only flips to "fire crit" when the server is also about
    // to see vy < 0 — same alignment the old hkTickMovement build
    // achieved by sampling at the START of the tick.
    {
        int new_state = TVS_GROUND;
        if (player_on_ground(env, c, local_player))
        {
            new_state = TVS_GROUND;
        }
        else
        {
            double vy = 0.0;
            if (read_velocity_y(env, c, local_player, vy))
            {
                if (vy <= DESCENT_VELOCITY_THRESHOLD)
                {
                    // Instant descent frame
                    new_state = TVS_DESCENDING;
                }
                else
                {
                    // Anything else (rising, exactly at apex, or one
                    // tick past it but not yet) → hold and wait.
                    new_state = TVS_ASCENDING;
                }
            }
            else
            {
                // Couldn't read velocity → strict mode: hold.
                new_state = TVS_ASCENDING;
            }
        }
        g_tick_vert_state.store(new_state, std::memory_order_release);
    }

    // ── Step 2: world-tick-rate dispatch fallback ────────────────────
    // If the render-thread dispatcher is alive and ticked recently,
    // skip — render runs at 60-360 Hz and gives sub-tick reaction time.
    unsigned long long now    = GetTickCount64();
    unsigned long long render = g_last_render_dispatch_ms.load(std::memory_order_acquire);
    bool render_alive = (render != 0) && (now - render <= RENDER_HEARTBEAT_MS);

    if (!render_alive)
    {
        try_trigger_attack(env, local_player);
    }
}

static void JNICALL hkHandleInputEvents(JNIEnv* env, jobject thiz)
{
    // DISABLED — never attached (hooking handleInputEvents breaks all input).
    // Left here so shutdown/detach symmetry is preserved.
    if (ORIG_handleInputEvents && g_minecraft_class)
    {
        env->CallNonvirtualVoidMethod(thiz, g_minecraft_class, ORIG_handleInputEvents);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

bool is_active() { return g_attached; }

unsigned long long debug_hook_fire_count()       { return g_dbg_hook_fires.load(std::memory_order_relaxed); }
unsigned long long debug_attack_dispatch_count() { return g_dbg_attacks.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_disabled()      { return g_dbg_skipped_disabled.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_no_target()     { return g_dbg_skipped_no_target.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_validation()    { return g_dbg_skipped_validation.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_cooldown()      { return g_dbg_skipped_cooldown.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_crit()          { return g_dbg_skipped_crit.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_shield()        { return g_dbg_skipped_shield.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_using_item()    { return g_dbg_skipped_using_item.load(std::memory_order_relaxed); }
unsigned long long debug_skipped_post_use()      { return g_dbg_skipped_post_use.load(std::memory_order_relaxed); }
unsigned long long debug_dispatch_failed()       { return g_dbg_dispatch_failed.load(std::memory_order_relaxed); }
unsigned long long debug_last_attack_tick_ms()   { return g_dbg_last_attack_ms.load(std::memory_order_relaxed); }

unsigned int consume_pending_on_hit()
{
    return g_pending_on_hit.exchange(0, std::memory_order_acq_rel);
}

unsigned int consume_pending_wtap_hit()
{
    return g_pending_wtap_hit.exchange(0, std::memory_order_acq_rel);
}

void on_entity_attacked(JNIEnv* env, jobject target)
{
    const uint64_t now = GetTickCount64();
    g_last_combat_time_ms.store(now, std::memory_order_release);
    g_last_moving_time_ms.store(now, std::memory_order_release);
    g_last_attack_time_ms = now;
    if (!env || !target || !triggerbot::settings::enabled || (!triggerbot::settings::focus_mode && !triggerbot::settings::only_on_hit)) return;
    triggerbot::init_cache(env);
    const auto& c = triggerbot::get_cache();
    if (!c.minecraft_world_fid || !gate_target_type(env, c, target) ||
        ::vanta::social::isFriend(env, target)) return;
    LocalRef minecraft(env, resolve_minecraft(env, c));
    if (!minecraft) return;
    if (triggerbot::settings::check_teams && c.minecraft_player_fid) {
        LocalRef local_player(env, env->GetObjectField(minecraft.get(), c.minecraft_player_fid));
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (local_player && ::vanta::social::isTeammate(env, local_player.get(), target)) return;
    }
    LocalRef world(env, env->GetObjectField(minecraft.get(), c.minecraft_world_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!world) return;
    {
        std::lock_guard lock(g_visual_focus_mx);
        if (!g_focused_entity || env->IsSameObject(g_focused_entity, target) != JNI_TRUE ||
            !g_focused_world || env->IsSameObject(g_focused_world, world.get()) != JNI_TRUE) {
            auto entityRef = env->NewGlobalRef(target);
            if (env->ExceptionCheck()) env->ExceptionClear();
            auto worldRef = entityRef ? env->NewGlobalRef(world.get()) : nullptr;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!entityRef || !worldRef) {
                if (entityRef) env->DeleteGlobalRef(entityRef);
                if (worldRef) env->DeleteGlobalRef(worldRef);
                return;
            }
            if (g_focused_entity) env->DeleteGlobalRef(g_focused_entity);
            if (g_focused_world) env->DeleteGlobalRef(g_focused_world);
            g_focused_entity = entityRef;
            g_focused_world = worldRef;
        }
    }
    refresh_visual_focus(env, target);
}
void notify_disabled()
{
    g_was_using_item_last_tick = false;
    g_post_use_release_ms      = 0;
    g_using_item_start_ms      = 0;
    g_was_eating               = false;
    g_post_eat_release_ms      = 0;
    g_last_combat_time_ms.store(0, std::memory_order_release);
    g_last_moving_time_ms.store(0, std::memory_order_release);

    if (vanta::jvm::ctx().vm)
    {
        JNIEnv* env = vanta::jvm::thisThreadEnv();
        if (env)
        {
            clear_focused_entity(env);
            clear_visual_focus(env);
            clear_last_target(env);
        }
    }
    g_consecutive_hits      = 0;
    g_current_random_offset = 0.0;
    g_focus_level           = 1.0;
    g_miss_until_ms         = 0;
    g_rot_init              = false;
}

bool init()
{
    if (g_attached) { TB_ONCE("init", "init() called, already attached"); return true; }
    if (!vanta::jvm::ctx().vm) { TB_ONCE("init", "init() failed: ctx().vm is null"); return false; }

    auto env = vanta::jvm::thisThreadEnv();
    auto jvm = vanta::jvm::ctx().vm.load();
    if (!env || !jvm) { TB_ONCE("init", "init() failed: env=%p vm=%p", (void*)env, (void*)jvm); return false; }

    TB_ONCE("init", "calling JNIHook_Init(vm=%p)", (void*)jvm);
    jnihook_result_t r = JNIHook_Init(jvm);
    TB_ONCE("init", "JNIHook_Init returned r=%d (0=OK, 3=ADD_JVMTI_CAPS_ERR)", (int)r);
    if (r != JNIHOOK_OK && r != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        TB_ONCE("init", "JNIHook_Init fatal error — cannot install doAttack hook");
        return false;
    }

    // ── Probe: ¿el host tiene mixins de bytecode sobre clases críticas? ──
    // Solo informativo. Las llamadas JNIHook_Attach abajo están envueltas en
    // SEH independientemente del resultado del probe, así que el comportamiento
    // es idéntico se haga el probe o no. Útil para diagnosticar Feather/Lunar
    // en los logs.
    bool hostile = false;
    if (hostile) {
        vanta::modules::triggerbot_hook::log_("triggerbot_hook: host has class mixins — JNIHook_Attach calls "
                   "are SEH-protected; doAttack hook may fall back to render-rate "
                   "dispatch only on aggressive-mixin hosts");
    }

    // --- Hook 1: ClientPlayerEntity.tickMovement — REMOVED ---
    //
    // The old build hooked class_746.method_6007 (tickMovement) via
    // JNIHook so it could (a) stash a server-tick-aligned snapshot of
    // the player's vertical state for the Priorize Crits gate and
    // (b) drive the trigger validation+dispatch path at 20 Hz as a
    // fallback when the render thread isn't pumping.
    //
    // Under MC 1.21.7+ (JVM 21 with stricter bytecode verification),
    // JNIHook's RetransformClasses on tickMovement produces a class
    // copy whose CallNonvirtualVoidMethod silently no-ops — symptom:
    // the local player freezes in place because tickMovement is what
    // processes input, applies physics, syncs sprint state, etc.
    //
    // Both responsibilities are now driven from
    // throwpot::hooked_sendMovementPackets() → triggerbot_hook::
    // tick_from_movement_packets(). sendMovementPackets fires at the
    // same 20 Hz cadence on the same game thread, immediately AFTER
    // tickMovement, so the snapshot timing and dispatch ordering are
    // functionally identical. Priorize Crits behaves exactly the
    // same as before; the only thing that changes is that we read
    // the vertical state one nanosecond later, after the Flying
    // packet for that tick has been built but before it's sent.
    ORIG_tickMovement = nullptr;
    g_tick_method_id  = nullptr;
    {
        // Still need a global ref to class_746 so future code that
        // wants to call ClientPlayerEntity methods non-virtually
        // (currently nothing — but kept for symmetry with shutdown())
        // doesn't crash on null.
        jclass cpe = vanta::jvm::findClassAnyLoader(env,
            sdk::mappings::clientplayerentity_class_sig);
        if (cpe) {
            g_clientplayer_class = (jclass)env->NewGlobalRef(cpe);
            env->DeleteLocalRef(cpe);
        }
    }

    // --- Hook 2: MinecraftClient.doAttack (class_310) ---
    // class_310 es el target principal de mixins agresivos en Feather
    // (Discord overlay, HUD scaling, focus, etc.). Si JNIHook_Attach
    // falla aquí, NO abortamos init(): el dispatch de attack lo cubre el
    // render-thread heartbeat (hkSwapBuffers → tick_from_render →
    // tick_pipeline) que ya existe. El triggerbot sigue plenamente
    // funcional con solo Hook 1 + render dispatch en Feather. En
    // Lunar/Vanilla/Modrinth este hook 2 instala normal y mejora el
    // timing al world-tick rate como antes.
    jclass mc_cls = vanta::jvm::findClassAnyLoader(env, sdk::mappings::minecraftclass_sig);
    if (mc_cls)
    {
        jmethodID da_mid = env->GetMethodID(mc_cls,
            sdk::mappings::do_attack_name,
            sdk::mappings::do_attack_sig);
        if (env->ExceptionCheck()) env->ExceptionClear();

        if (da_mid)
        {
            jnihook_result_t r2 = JNIHOOK_ERR_JVMTI_OPERATION;
            __try {
                r2 = JNIHook_Attach(da_mid,
                    reinterpret_cast<void*>(hkDoAttack),
                    &ORIG_doAttack);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                vanta::modules::triggerbot_hook::log_("triggerbot_hook: JNIHook_Attach(doAttack) SEH crash "
                           "code=0x%08lx — host has aggressive class_310 mixins "
                           "(Feather?). Falling back to render-rate dispatch.",
                           (unsigned long)GetExceptionCode());
                r2 = JNIHOOK_ERR_JVMTI_OPERATION;
            }

            if (r2 == JNIHOOK_OK)
            {
                jclass gref = (jclass)env->NewGlobalRef(mc_cls);
                if (gref) {
                    g_minecraft_class    = gref;
                    g_doattack_method_id = da_mid;
                } else {
                    // Sin GlobalRef no podemos llamar al original — detach
                    // limpio y deja ORIG_doAttack a nullptr para que el
                    // callback hkDoAttack rebote (defensive guard ya existe
                    // en línea ~709).
                    JNIHook_Detach(da_mid);
                    ORIG_doAttack = nullptr;
                    vanta::modules::triggerbot_hook::log_("triggerbot_hook: NewGlobalRef(class_310) failed — "
                               "dropping doAttack hook (Hook 1 + render dispatch "
                               "still active)");
                }
            }
            else
            {
                // Asegura ORIG_doAttack=null para que el callback (si llegase
                // por alguna razón) rebote sin crash.
                ORIG_doAttack = nullptr;
                vanta::modules::triggerbot_hook::log_("triggerbot_hook: doAttack attach failed (r=%d) — "
                           "continuing with tickMovement-only + render dispatch",
                           (int)r2);
            }
        }
        else
        {
            vanta::modules::triggerbot_hook::log_("triggerbot_hook: doAttack methodID lookup failed — "
                       "skipping hook 2 (render dispatch still works)");
        }

        // v124b: Hook 3 — MinecraftClient.handleBlockBreaking(Z)V (method_1590).
        // Necesario para suprimir START_DIGGING cuando silent aim tiene target
        // publicado → evita Grim RotationBreak + MultiActionsF flags. Requiere
        // el mismo g_minecraft_class que doAttack (mismo lookup mc_cls). Attach
        // solo si doAttack tuvo éxito (necesitamos g_minecraft_class válido).
        if (g_minecraft_class)
        {
            jmethodID hbb_mid = env->GetMethodID(mc_cls, "method_1590", "(Z)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (hbb_mid)
            {
                jnihook_result_t r3 = JNIHOOK_ERR_JVMTI_OPERATION;
                __try {
                    r3 = JNIHook_Attach(hbb_mid,
                        reinterpret_cast<void*>(hkHandleBlockBreaking),
                        &ORIG_handleBlockBreaking);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    vanta::modules::triggerbot_hook::log_("triggerbot_hook: JNIHook_Attach(handleBlockBreaking) "
                               "SEH crash code=0x%08lx — skipping. Silent aim will "
                               "still work, but Grim may flag RotationBreak on block "
                               "clicks.", (unsigned long)GetExceptionCode());
                    r3 = JNIHOOK_ERR_JVMTI_OPERATION;
                }
                if (r3 == JNIHOOK_OK) {
                    g_hbb_method_id = hbb_mid;
                    vanta::modules::triggerbot_hook::log_("triggerbot_hook: hkHandleBlockBreaking attached OK");
                } else {
                    ORIG_handleBlockBreaking = nullptr;
                    vanta::modules::triggerbot_hook::log_("triggerbot_hook: handleBlockBreaking attach failed "
                               "(r=%d) — silent aim may flag RotationBreak", (int)r3);
                }
            }
            else
            {
                vanta::modules::triggerbot_hook::log_("triggerbot_hook: handleBlockBreaking methodID lookup "
                           "failed (method_1590 missing?) — silent aim may flag "
                           "RotationBreak on block clicks");
            }
        }

        env->DeleteLocalRef(mc_cls);
    }
    else
    {
        vanta::modules::triggerbot_hook::log_("triggerbot_hook: class_310 not found — skipping hook 2 "
                   "(render dispatch still works)");
    }

    // Hook 3 (handleInputEvents) intentionally NOT attached — breaks input.
    ORIG_handleInputEvents = nullptr;
    g_input_method_id      = nullptr;

    g_attached = true;
    TB_ONCE("init", "init() finished — g_attached=true, ORIG_doAttack=%p, mc_class=%p. "
                    "Render-path dispatch is what actually fires the SendInput; "
                    "the doAttack hook is only a gate.",
            (void*)ORIG_doAttack, (void*)g_minecraft_class);
    return true;
}

void shutdown()
{
    if (!g_attached)
    {
        ORIG_tickMovement       = nullptr;
        ORIG_doAttack           = nullptr;
        ORIG_handleInputEvents  = nullptr;
        ORIG_handleBlockBreaking = nullptr;
        g_tick_method_id        = nullptr;
        g_doattack_method_id    = nullptr;
        g_input_method_id       = nullptr;
        g_hbb_method_id         = nullptr;
        return;
    }

    auto detach = [](jmethodID mid, const char* label) {
        if (!mid) { vanta::modules::triggerbot_hook::log_("%s: mid null, skipped", label); return; }
        try {
            jnihook_result_t r = JNIHook_Detach(mid);
            vanta::modules::triggerbot_hook::log_("%s: JNIHook_Detach r=%d", label, (int)r);
        } catch (...) {
            vanta::modules::triggerbot_hook::log_("%s: JNIHook_Detach threw", label);
        }
    };

    detach(g_input_method_id,    "triggerbot_hook::shutdown handleInputEvents");
    detach(g_hbb_method_id,      "triggerbot_hook::shutdown handleBlockBreaking");
    detach(g_doattack_method_id, "triggerbot_hook::shutdown doAttack");
    detach(g_tick_method_id,     "triggerbot_hook::shutdown tickMovement");

    if (vanta::jvm::ctx().vm)
    {
        try
        {
            auto env = vanta::jvm::thisThreadEnv();
            if (env)
            {
                if (g_clientplayer_class) env->DeleteGlobalRef(g_clientplayer_class);
                if (g_minecraft_class)    env->DeleteGlobalRef(g_minecraft_class);
                clear_focused_entity(env);
                clear_visual_focus(env);
                if (g_last_target)        env->DeleteGlobalRef(g_last_target);
            }
        }
        catch (...) { vanta::modules::triggerbot_hook::log_("triggerbot_hook::shutdown: DeleteGlobalRef threw"); }
    }

    g_clientplayer_class    = nullptr;
    g_minecraft_class       = nullptr;
    ORIG_tickMovement       = nullptr;
    ORIG_doAttack           = nullptr;
    ORIG_handleInputEvents  = nullptr;
    ORIG_handleBlockBreaking = nullptr;
    g_tick_method_id        = nullptr;
    g_doattack_method_id    = nullptr;
    g_input_method_id       = nullptr;
    g_hbb_method_id         = nullptr;
    g_attached              = false;
    // Clear silent aim spoof state on shutdown (release el global ref si queda).
    {
        jobject old = g_silentaim_spoof_target.exchange(nullptr, std::memory_order_acq_rel);
        if (old && vanta::jvm::ctx().vm) {
            try {
                auto env = vanta::jvm::thisThreadEnv();
                if (env) env->DeleteGlobalRef(old);
            } catch (...) {}
        }
        g_silentaim_active.store(false, std::memory_order_release);
    }
    { std::lock_guard lock(g_synthetic_click_mx); g_synthetic_click.clear(); }
    { std::lock_guard lock(g_recharge_mx); g_recharge.clear(); }
    g_pending_on_hit.store(0);
    g_rot_init = false;

    g_was_using_item_last_tick = false;
    g_post_use_release_ms      = 0;
    g_using_item_start_ms      = 0;
    g_was_eating               = false;
    g_post_eat_release_ms      = 0;
    g_last_attack_time_ms      = 0;

    g_last_target           = nullptr;
    g_target_acquired_ms    = 0;
    g_consecutive_hits      = 0;
    g_current_random_offset = 0.0;
    g_focus_level           = 1.0;
    g_miss_until_ms         = 0;
}

jobject visual_focus_target(JNIEnv* env)
{
    if (!env || !triggerbot::settings::enabled || !triggerbot::settings::focus_mode ||
        !triggerbot::settings::show_focus_visuals) return nullptr;
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    jobject active_ref = g_focused_entity ? g_focused_entity : g_visual_focus_entity;
    if (!active_ref) return nullptr;
    const auto& c = triggerbot::get_cache();
    if (target_is_dead(env, c, active_ref)) {
        if (g_visual_focus_entity) { env->DeleteGlobalRef(g_visual_focus_entity); g_visual_focus_entity = nullptr; }
        return nullptr;
    }
    const float seconds = triggerbot::settings::focus_visual_seconds;
    if (seconds > 0.0f && !g_focused_entity && triggerbot::focus_visual::expired(GetTickCount64(), g_visual_focus_started_ms, seconds)) {
        if (g_visual_focus_entity) { env->DeleteGlobalRef(g_visual_focus_entity); g_visual_focus_entity = nullptr; }
        return nullptr;
    }
    return env->NewLocalRef(active_ref);
}

bool is_focus_target_locked(JNIEnv* env, jobject ent)
{
    if (!env || !ent) return false;
    const auto& c = triggerbot::get_cache();
    if (target_is_dead(env, c, ent)) return false;
    std::lock_guard<std::mutex> lock(g_visual_focus_mx);
    return g_focused_entity && env->IsSameObject(ent, g_focused_entity) == JNI_TRUE;
}

}
}
}
