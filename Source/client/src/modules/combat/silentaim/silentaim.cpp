#include "core/NativeCallbacks.h"
// silentaim.cpp -- JNI reflection cache, the rotation hook, and the probe that
// decides whether that hook may be trusted on this host.
//
// The targeting and the controller live in silentaim_tick.cpp.

#include "silentaim.h"
#include "silentaim_tick.h"
#include <algorithm>

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <jnihook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <random>
#include <vector>
#include <mutex>
#include <windows.h>   // SEH around every JNIHook_Attach -- see install_rotation_hook

#include "modules/misc/spectate/spectate.h"   // the camera loan, see below

namespace vanta::modules::silentaim {

static jni_cache  g_cache;
static std::mutex s_cache_mx;

const jni_cache& get_cache()     { return g_cache; }
jni_cache&       mutable_cache() { return g_cache; }

// â”€â”€ small reflection helpers (same shape the other modules use) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static bool cache_class(JNIEnv* env, const char* sig, jclass& out)
{
    if (!sig) return false;
    jclass local = vanta::jvm::findClassAnyLoader(env, sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    out = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return out != nullptr;
}

static jmethodID cache_method(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

static jfieldID cache_field(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID f = env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return f;
}

// Try the layout the mappings claim first, then the other one. bind_mappings()
// is not wired in this tree, so is_mojang_layout is always false and a
// Mojang-mapped host would otherwise resolve nothing. The triggerbot resolves
// its rotation fields exactly this way.
static jfieldID cache_field_either(JNIEnv* env, jclass cls,
                                   const char* yarn, const char* mojang,
                                   const char* sig)
{
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    jfieldID f = cache_field(env, cls, mojang_first ? mojang : yarn, sig);
    if (!f) f = cache_field(env, cls, mojang_first ? yarn : mojang, sig);
    return f;
}

static jmethodID cache_method_either(JNIEnv* env, jclass cls,
                                     const char* yarn, const char* mojang,
                                     const char* sig)
{
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    jmethodID m = cache_method(env, cls, mojang_first ? mojang : yarn, sig);
    if (!m) m = cache_method(env, cls, mojang_first ? yarn : mojang, sig);
    return m;
}

// â”€â”€ the managed rotation, shared with the hook â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Written by the pump, read by the hook. Both run on the game thread, so the
// atomics are about publication order rather than contention.
static std::atomic<bool>  g_rot_active{ false };
static std::atomic<float> g_rot_yaw   { 0.f };
static std::atomic<float> g_rot_pitch { 0.f };
static std::atomic<float> g_rot_offset{ 0.f };   // degrees from the real view

// -- the action lease ---------------------------------------------------------
//
// Written by the pump (request/release), stepped and consumed by the hooks.
// Everything is on the game thread; the atomics above are the only things the
// render side ever reads.
//
// Lifetime of one lease:
//   pump      request_action_rotation(owner, target)      -> granted, refreshed each frame
//   pre-tick  one randomised step toward the target, snap, open the movement
//             frame, on_ready(ctx) with the APPLIED angle    -> Wait / Done / Abort
//   smp hook  original sends the flying carrying that angle;
//             if Done was returned this tick: drop the owner, hold the angle
//             for a random 1..4 ticks, then begin easing
//   pump      (still has a target) re-leases -> continues from the current angle
//   ...       (no target) -> the easing walks the sent angle back to the view
//             with the same randomised stepping, then the spoof goes off.
static const void* g_action_owner = nullptr;
static unsigned long long g_action_until = 0;
static float g_action_yaw = 0.f, g_action_pitch = 0.f;           // stepped (unsnapped) angle
static float g_action_target_yaw = 0.f, g_action_target_pitch = 0.f;
static bool  g_action_model = true;
static bool  g_action_easing = false;              // walking back to the real view
static bool  g_action_release_after_send = false;  // Done this tick: release after the flying
static bool  g_action_release_pending = false;     // release() called from inside a dispatch
static bool  g_in_action_dispatch = false;
static unsigned g_tick_id = 0;                     // once per client tick
static unsigned g_action_ran_tick = ~0u;           // tick on which on_ready last ran
static unsigned g_action_done_tick = ~0u;          // tick on which on_ready returned Done (packets went out)
static unsigned g_action_hold_until = 0;           // easing waits until this tick (see (d) below)
// (h) the settle: true once the lease has landed on its target. From then on
// the exact sent pair is republished tick after tick -- no re-snap against a
// drifting real view, no float creep -- until the target changes or the lease
// ends, so the wire tracker below can report the applied angle as settled.
static bool     g_action_settled = false;
// Whether the lease was settled when a Done released it: a re-lease from the
// pump that continues the hold with the SAME target (the pump's hold() after
// the click) then keeps the settle and republishes the exact pair instead of
// re-running the landing step, whose snap + guard could publish a value one
// lattice count off -- an extra look and an extra settling tick per action.
static bool     g_action_settled_at_release = false;

// -- the rotation on the wire ---------------------------------------------------
//
// What Grim holds. Read around the original in hkSendMovementPackets from
// vanilla's own bookkeeping (lastYaw/lastPitch are copied from the fields
// exactly when a look goes out, ticksSinceLastPositionPacketSent is zeroed
// exactly when a position goes out), so this is the truth about the packets,
// not a guess from our own state. Never written: AimDuplicateLook lives on
// vanilla's comparator against those two fields. Seeded from the player's
// rotation when the hook is installed.
static float    g_wire_yaw = 0.f, g_wire_pitch = 0.f;             // the last LOOK sent
static float    g_wire_prev_yaw = 0.f, g_wire_prev_pitch = 0.f;   // the look before it
static bool     g_wire_have = false, g_wire_have_prev = false;
static unsigned g_wire_tick = ~0u;                                 // tick of the last flying of any kind
static bool     g_wire_fields_warned = false;

// Registered callbacks. on_tick has to outlive the lease (the slot restore
// after an action happens on a LATER tick with no lease), so registrations
// persist until the owner unregisters. Two explosive modules can be on at
// once, hence a small table rather than one slot.
struct ActionRegistration { const void* owner = nullptr; ActionCallbacks cb; };
static ActionRegistration g_action_regs[4];

static ActionRegistration* find_registration(const void* owner)
{
    for (auto& r : g_action_regs) if (r.owner == owner) return &r;
    return nullptr;
}

static bool action_live()     { return g_action_owner && GetTickCount64() <= g_action_until; }
// Lease OR easing: the wire is carrying an action rotation either way, and the
// aura's own controller has to stay out until the easing has landed.
static bool action_spoofing() { return action_live() || g_action_easing; }

// -- how the sent angle moves: the Vulcan rules ---------------------------------
//
// Vulcan's rotation checks are statistical, and each of these rules removes
// one statistic a scripted turn would otherwise leave behind:
//
//   (a) no synthetic rotation when the real view already validates the action
//       -- the owner leases with target == real, so nothing but the player's
//       own angle goes out (done in ExplosiveModules.cpp: realViewValid).
//   (b) the aim point is drawn at random inside the valid region per action,
//       so the same target never yields the same angle twice (exact target
//       repetition; also in ExplosiveModules.cpp).
//   (c) the speed cap is drawn per lease (18..34 deg/tick) and each tick uses
//       a random 55..100 % of it, with an ease-out as the remainder shrinks:
//       consecutive deltas are never identical and never a constant speed.
//   (d) after Done the angle holds for a random 1..4 ticks before it walks
//       back, with the same randomised stepping: a turn is never mirrored by
//       an equal-and-opposite snap on the very next packet.
//   (e) every sent angle is snapped to the mouse lattice, pitch clamped, yaw
//       kept on the real yaw's winding (publish_action_rotation).
//   (f) the easing converges on the LIVE real view each tick: when the player
//       moved their mouse during the lease the sent angle blends toward where
//       they look now instead of retracing the path it came by.
//   (g) after the snap, a stepped delta is never bit-equal to the previous
//       non-zero stepped delta nor to its negation: (c) makes that a rare
//       coincidence of two random draws on the lattice, this makes it
//       impossible. A hit is pushed one lattice count further along its own
//       direction (still real + k*L, so still on the player's lattice), and
//       it happens BEFORE on_ready sees the angle, so what is validated is
//       what goes out.
//   (h) once landed, the SAME pair goes out until the owner acts or the
//       target moves: a settled aim is bit-stable across ticks, which is what
//       lets a block click wait for the flying that carries it (wire_settled
//       in ActionContext) instead of racing it. The hold after a Done is the
//       same exact pair, for the same reason.
static std::mt19937 g_action_rng{ std::random_device{}() };
static float action_rnd(float lo, float hi)
{
    return std::uniform_real_distribution<float>(lo, hi)(g_action_rng);
}
static constexpr float kActionCapMin = 18.f, kActionCapMax = 34.f;   // crystal cap range
static constexpr float kAnchorCapMin = 28.f, kAnchorCapMax = 48.f;   // anchor cap range

static float action_cap_min() { return g_action_model ? kAnchorCapMin : kActionCapMin; }
static float action_cap_max() { return g_action_model ? kAnchorCapMax : kActionCapMax; }
static float g_action_speed_cap = 27.f;                              // this lease's / this easing's cap

// (g) sent-angle history: the last value published for the wire, and the last
// NON-ZERO delta a step produced (the hold publishes too, but a hold's deltas
// are the player's own mouse and must not shadow the step that preceded it --
// the first step of the walk home is compared against the last step out).
static float g_sent_prev_yaw = 0.f, g_sent_prev_pitch = 0.f;
static bool  g_sent_have_prev = false;
static float g_sent_last_dyaw = 0.f, g_sent_last_dpitch = 0.f;

static void reset_sent_history(float yaw, float pitch, bool have)
{
    g_sent_prev_yaw = yaw; g_sent_prev_pitch = pitch;
    g_sent_have_prev = have;
    g_sent_last_dyaw = 0.f; g_sent_last_dpitch = 0.f;
}

// -- the mouse lattice from the option ----------------------------------------
static float g_mouse_lattice  = 0.f;      // 0: unknown
static bool  g_mouse_smooth   = false;
static bool  g_sens_unresolved_logged = false;
static unsigned long long g_mouse_lattice_at = 0;

float mouse_lattice_deg() { return g_mouse_smooth ? 0.f : g_mouse_lattice; }

static jobject fetch_options(JNIEnv* env);
static jobject fetch_minecraft(JNIEnv* env);
// Set the first time the triggerbot's pre-tick callback reaches us. Defined
// here because the sendMovementPackets hook keys its fallback on it.
static std::atomic<bool> g_pretick_live{ false };

// Re-reads GameOptions once a second. mc may be null, in which case the static
// instance is fetched -- the sendMovementPackets fallback has no
// MinecraftClient in hand.
static void refresh_mouse_lattice(JNIEnv* env, jobject mc)
{
    const auto now = GetTickCount64();
    if (now - g_mouse_lattice_at < 1000) return;
    g_mouse_lattice_at = now;

    if (!g_cache.mc_options_fid
        || (!g_cache.options_get_sensitivity_mid && !g_cache.options_mouse_sensitivity_fid)
        || !g_cache.option_get_value_mid || !g_cache.double_value_mid) {
        if (!g_sens_unresolved_logged) {
            g_sens_unresolved_logged = true;
            VLOG_WARN("silentaim", "lattice: sensitivity unresolved, using fallback");
        }
        if (g_mouse_lattice <= 0.f) g_mouse_lattice = 0.15f;
        return;
    }

    jobject options = nullptr;
    if (mc) {
        options = env->GetObjectField(mc, g_cache.mc_options_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); options = nullptr; }
    } else {
        options = fetch_options(env);
    }
    if (!options) {
        if (g_mouse_lattice <= 0.f) g_mouse_lattice = 0.15f;
        return;
    }

    jobject opt = nullptr;
    if (g_cache.options_get_sensitivity_mid) {
        opt = env->CallObjectMethod(options, g_cache.options_get_sensitivity_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); opt = nullptr; }
    }
    if (!opt && g_cache.options_mouse_sensitivity_fid) {
        opt = env->GetObjectField(options, g_cache.options_mouse_sensitivity_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); opt = nullptr; }
    }

    jobject boxed = opt ? env->CallObjectMethod(opt, g_cache.option_get_value_mid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); boxed = nullptr; }
    if (boxed) {
        const double s = env->CallDoubleMethod(boxed, g_cache.double_value_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        else if (std::isfinite(s)) {
            // Bit-exact with Mouse.updateMouse: the constants there are the
            // float literals 0.6f/0.2f widened to double, the cube is done in
            // double, times 8.0, then changeLookDirection multiplies the
            // (float) count by 0.15f.
            const double d = s * static_cast<double>(0.6f) + static_cast<double>(0.2f);
            const double f = d * d * d * 8.0;
            g_mouse_lattice = static_cast<float>(f) * 0.15f;
        }
        env->DeleteLocalRef(boxed);
    }
    if (opt) env->DeleteLocalRef(opt);

    if (g_mouse_lattice <= 0.f) g_mouse_lattice = 0.15f;

    if (g_cache.options_smooth_camera_fid) {
        const jboolean smooth = env->GetBooleanField(options, g_cache.options_smooth_camera_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else g_mouse_smooth = (smooth == JNI_TRUE);
    }
    env->DeleteLocalRef(options);
}

// Lattice actually used for the snap: the option when readable, the solver's
// consensus when not, guaranteed 0.15 deg fallback so we never emit unquantized
// angles or random float nudges (nothing while smooth camera is on).
static float effective_lattice()
{
    if (g_mouse_smooth) return 0.f;
    if (g_mouse_lattice > 0.f) return g_mouse_lattice;
    const float obs = silentaim_tick::observed_lattice_deg();
    if (obs > 0.f) return obs;
    return 0.15f;
}

// (e) the snap quantises the OFFSET from the real angle (snap_action_angle:
// real + round((v - real) / L) * L), so every published value sits on the
// player's own lattice whatever winding the real yaw is on; the pitch is
// clamped after the snap. `stepped` is true when a step produced the value
// (lease approach or easing), false for the hold -- only steps run (g).
static void publish_action_rotation(float real_yaw, float real_pitch, bool stepped)
{
    const float L = effective_lattice();
    float send_yaw   = silentaim_tick::snap_action_angle(g_action_yaw, real_yaw, L);
    float send_pitch = std::clamp(silentaim_tick::snap_action_angle(g_action_pitch, real_pitch, L), -90.f, 90.f);

    if (stepped && g_sent_have_prev) {
        // (g) no exact repeat, no exact mirror. Compared in lattice counts
        // when the lattice is known (the values are on it, so equality of
        // counts IS equality of the sent deltas); bit-equality otherwise.
        auto guard = [&](float& v, float prev, float& last_d, float lo, float hi) {
            float d = v - prev;
            if (d == 0.f) return;                        // no rotation goes out on a 0 delta
            bool same;
            if (L > 0.f) {
                const long a = std::lround(d / L), b = std::lround(last_d / L);
                same = (b != 0) && (a == b || a == -b);
            } else {
                same = (last_d != 0.f) && (d == last_d || d == -last_d);
            }
            if (same) {
                const float nudge = L > 0.f ? L : action_rnd(0.01f, 0.05f);
                v = std::clamp(v + (d > 0.f ? nudge : -nudge), lo, hi);
                d = v - prev;
            }
            last_d = d;
        };
        guard(send_yaw,   g_sent_prev_yaw,   g_sent_last_dyaw,   -1e9f, 1e9f);
        guard(send_pitch, g_sent_prev_pitch, g_sent_last_dpitch, -90.f, 90.f);
    }
    g_sent_prev_yaw = send_yaw; g_sent_prev_pitch = send_pitch; g_sent_have_prev = true;

    g_rot_yaw.store(send_yaw, std::memory_order_relaxed);
    g_rot_pitch.store(send_pitch, std::memory_order_relaxed);
    g_rot_offset.store(std::max(std::fabs(std::remainder(send_yaw - real_yaw, 360.f)),
                                std::fabs(send_pitch - real_pitch)), std::memory_order_relaxed);
    g_rot_active.store(true, std::memory_order_release);
}

// (h) the exact pair last published goes out again. No snap, no guard, no
// arithmetic: the value Grim already holds is the value it keeps holding.
static void republish_sent_rotation(float real_yaw, float real_pitch)
{
    if (!g_sent_have_prev) { publish_action_rotation(real_yaw, real_pitch, false); return; }
    g_rot_yaw.store(g_sent_prev_yaw, std::memory_order_relaxed);
    g_rot_pitch.store(g_sent_prev_pitch, std::memory_order_relaxed);
    g_rot_offset.store(std::max(std::fabs(std::remainder(g_sent_prev_yaw - real_yaw, 360.f)),
                                std::fabs(g_sent_prev_pitch - real_pitch)), std::memory_order_relaxed);
    g_rot_active.store(true, std::memory_order_release);
}

// One tick of motion toward the target (lease) or back to the view (easing).
// Runs once per tick, at the pre-tick point when it is live, otherwise from
// the sendMovementPackets hook before the spoof is written.
static void step_action_rotation(float real_yaw, float real_pitch)
{
    if (!action_spoofing()) return;

    float yaw_delta, pitch_delta;
    bool  landing = false;   // this step reaches the target exactly
    if (action_live()) {
        // (h) landed on this target already: the same pair again, whatever
        // the real view did meanwhile. The owner is waiting for the wire.
        if (g_action_settled) { republish_sent_rotation(real_yaw, real_pitch); return; }
        // remainder() keeps the unbounded winding of the real yaw -- no
        // 360 step ever appears on the wire (AimModulo360).
        yaw_delta   = std::remainder(g_action_target_yaw - g_action_yaw, 360.f);
        pitch_delta = std::clamp(g_action_target_pitch, -90.f, 90.f) - g_action_pitch;
    } else {
        // (d) the hold: the angle the action was judged against stays on the
        // wire for a few ticks, as a hand rests after a click -- the exact
        // pair, so the flying behind the click carries what was validated.
        if (static_cast<int>(g_tick_id - g_action_hold_until) < 0) {
            republish_sent_rotation(real_yaw, real_pitch);
            return;
        }
        // (f) Easing converges on the NUMBER the real yaw holds right now,
        // like the aura's disengage: the spoof switches off onto that exact
        // value, and a view that moved meanwhile is simply the new target.
        yaw_delta   = real_yaw   - g_action_yaw;
        pitch_delta = real_pitch - g_action_pitch;
    }
    const float length = std::hypot(yaw_delta, pitch_delta);
    if (length > 0.f) {
        // (c) this tick's step: a random share of the lease's cap, an ease-out
        // (60..90 % of what is left) once the remainder fits inside one step,
        // and the exact remainder only for the last few degrees.
        // Anchor interactions are a short, ordered combo (place, charge,
        // explode). Give them a wider first step and a tighter ease-out so a
        // moving target does not make the lease spend several ticks circling
        // the same face. Crystals retain the slower general-purpose profile.
        const float cap  = g_action_speed_cap * action_rnd(g_action_model ? .72f : .55f, 1.0f);
        float       step = length;
        if (length > cap)      step = cap;
        else if (length > 4.f) step = length * action_rnd(g_action_model ? .75f : .6f, 0.9f);
        else                   landing = true;   // the exact remainder: this step lands
        const float scale = step / length;
        g_action_yaw   += yaw_delta * scale;
        g_action_pitch += pitch_delta * scale;
    } else {
        landing = true;
    }
    g_action_pitch = std::clamp(g_action_pitch, -90.f, 90.f);
    // (h) a live lease that has just landed publishes this one stepped value
    // (snap + guard as usual) and then holds it bit for bit from the next
    // tick on. The float add above may leave g_action_yaw a few ulp off the
    // target; that residue is deliberately never chased, the published pair
    // is what counts.
    if (action_live() && landing) g_action_settled = true;

    if (!action_live() && std::fabs(real_yaw - g_action_yaw) < 0.25f
                       && std::fabs(real_pitch - g_action_pitch) < 0.25f) {
        // Landed. Off the wire; the model is handed back by the hook's own
        // "!spoof" path on this same tick. From here the wire carries the
        // player's own rotation, so the step history starts over.
        g_action_easing = false;
        g_rot_active.store(false, std::memory_order_release);
        reset_sent_history(0.f, 0.f, false);
        return;
    }
    publish_action_rotation(real_yaw, real_pitch, true);
}

// Drops the owner. If a rotation is on the wire it eases back instead of
// cutting dead; if nothing was ever published, it simply goes quiet.
static void begin_action_release()
{
    g_action_owner = nullptr;
    g_action_release_after_send = false;
    g_action_settled_at_release = g_action_settled;
    g_action_settled = false;
    if (g_rot_active.load(std::memory_order_acquire)) {
        g_action_easing = true;
        // (d) hold 1..4 ticks, then walk home under a fresh cap so the way
        // back is never the way out played in reverse.
        g_action_hold_until = g_tick_id + 1u
            + static_cast<unsigned>(std::uniform_int_distribution<int>(0, 3)(g_action_rng));
        g_action_speed_cap  = action_rnd(action_cap_min(), action_cap_max());
    }
}

void release_action_rotation(const void* owner)
{
    if (!owner || g_action_owner != owner) return;
    if (g_in_action_dispatch) { g_action_release_pending = true; return; }
    begin_action_release();
}

void register_action_callbacks(const void* owner, const ActionCallbacks& cb)
{
    if (!owner) return;
    ActionRegistration* r = find_registration(owner);
    if (!r) r = find_registration(nullptr);
    if (r) { r->owner = owner; r->cb = cb; }
}

void unregister_action_callbacks(const void* owner)
{
    release_action_rotation(owner);
    if (auto* r = find_registration(owner)) { r->owner = nullptr; r->cb = ActionCallbacks{}; }
}

bool action_lease_held(const void* owner)
{
    return owner && g_action_owner == owner && action_live();
}

bool action_rotation_active() { return action_spoofing(); }

bool request_action_rotation(const void* owner, float real_yaw, float real_pitch,
                             float target_yaw, float target_pitch, bool model,
                             const ActionCallbacks* cb)
{
    if (!owner || !silent_rotation_available()) return false;
    if (!std::isfinite(target_yaw) || !std::isfinite(target_pitch)
        || !std::isfinite(real_yaw) || !std::isfinite(real_pitch)) return false;
    if (g_in_action_dispatch) return action_lease_held(owner);   // record nothing mid-dispatch
    if (action_live() && g_action_owner != owner) return false;

    // Keep the fallback solver fed from here too: the aura's pump only feeds
    // it while the SilentAim module is on.
    silentaim_tick::feed_lattice_observer(real_yaw, real_pitch);

    if (cb) {
        ActionRegistration* r = find_registration(owner);
        if (!r) r = find_registration(nullptr);
        if (r) { r->owner = owner; r->cb = *cb; }
    }

    if (!action_live()) {
        g_action_owner = owner;
        // Continuity: the first step must leave from the number that is on
        // the wire RIGHT NOW, whatever put it there.
        //   * our own easing after the previous Done -- carry on from the
        //     stepped angle (seeding from the view here is what would put a
        //     step onto the wire between two back-to-back actions);
        //   * the aura's managed rotation (g_rot_active without an easing) --
        //     carry on from the value it last published. Carrying the STALE
        //     g_action_yaw of a lease from minutes ago instead was a jump of
        //     however far the real yaw had wound since -- hundreds of degrees
        //     onto a wire whose previous delta was small, which is exactly
        //     the AimModulo360 shape (the initial 0 against a real yaw of
        //     -488 is the worst case: a 488 step);
        //   * nothing on the wire -- the player's own rotation, so the last
        //     sent value is the real one.
        // Every seed is on the real yaw's winding (the aura folds its value
        // onto it every tick, the easing converges on it), so the remainder
        // stepping below keeps the sent yaw within one turn of the view.
        // A re-lease inside the hold after a Done: the exact landed pair is
        // still what goes out each tick (republish), so the settle carries
        // over; the target compare below drops it again if the pump asks for
        // a different angle.
        const bool resume_settled = g_action_easing && g_action_settled_at_release
            && static_cast<int>(g_tick_id - g_action_hold_until) < 0 && g_sent_have_prev;
        if (g_action_easing) {
            /* keep g_action_yaw / g_action_pitch */
        } else if (g_rot_active.load(std::memory_order_acquire)) {
            g_action_yaw   = g_rot_yaw.load(std::memory_order_relaxed);
            g_action_pitch = g_rot_pitch.load(std::memory_order_relaxed);
            reset_sent_history(g_action_yaw, g_action_pitch, true);
        } else {
            g_action_yaw = real_yaw; g_action_pitch = real_pitch;
            reset_sent_history(real_yaw, real_pitch, true);
        }
        g_action_easing = false;
        g_action_release_after_send = false;
        g_action_settled = resume_settled;
        g_action_settled_at_release = false;
        // (c) a new lease, a new speed. A re-lease that continues an easing
        // keeps its angle but still draws afresh: two consecutive actions on
        // the same base then turn at different rates.
        // Set the profile before drawing the cap; the previous build selected
        // the old owner's profile for the first tick of every new anchor lease.
        g_action_model = model;
        g_action_speed_cap = action_rnd(action_cap_min(), action_cap_max());
    }
    g_action_until = GetTickCount64() + 250;
    // (h) a refresh with the SAME target keeps the settle; a new target (the
    // next step of a combo, or the aim point re-drawn) steps again.
    const float clamped_pitch = std::clamp(target_pitch, -90.f, 90.f);
    if (target_yaw != g_action_target_yaw || clamped_pitch != g_action_target_pitch)
        g_action_settled = false;
    g_action_target_yaw   = target_yaw;
    g_action_target_pitch = clamped_pitch;
    return true;
}

void set_managed_rotation(bool active, float yaw, float pitch, float offset_deg)
{
    if (action_spoofing()) return;
    g_rot_yaw.store(yaw,     std::memory_order_relaxed);
    g_rot_pitch.store(pitch, std::memory_order_relaxed);
    g_rot_offset.store(offset_deg, std::memory_order_relaxed);
    g_rot_active.store(active, std::memory_order_release);
}

// Runs the owner's callback with every guard the hook needs: a local frame,
// no C++ exception escaping into Minecraft, no Java exception left pending.
// A separate function on purpose -- the hooks use SEH elsewhere and MSVC will
// not mix __try with objects that have destructors.
static DWORD g_hook_thread = 0;
static bool  g_hook_thread_warned = false;
static bool  on_hook_thread()
{
    const DWORD me = GetCurrentThreadId();
    if (!g_hook_thread) g_hook_thread = me;
    if (g_hook_thread == me) return true;
    if (!g_hook_thread_warned) {
        g_hook_thread_warned = true;
        VLOG_WARN("silentaim", "action dispatch skipped: hook entered from another thread");
    }
    return false;
}

// Did vanilla already attack or use an item earlier in this tick? Both live
// in handleInputEvents ahead of the pre-tick point, so a slot packet or an
// interaction of ours would land BEHIND vanilla's ATTACK / USE_ITEM inside the
// same tick: PacketOrderE for the slot, PacketOrderJ / MultiActionsF for the
// interaction. The footprints are vanilla's own counters:
//   * MinecraftClient.itemUseCooldown: decremented at the very top of tick(),
//     set to 4 at the top of doItemUse -- so 4 means "used this tick" at any
//     point after handleInputEvents, both dispatch points included;
//   * PlayerEntity.lastAttackedTicks: reset to 0 by attackEntity (and by
//     PlayerEntity.attack), incremented unconditionally in PlayerEntity.tick,
//     which runs from world.tickEntities -- AFTER the pre-tick point (0 there)
//     and BEFORE sendMovementPackets (1 there). Verified on the 1.21.11
//     bytecode; the intermediary names are the same on 1.21.
// Unresolved fields read as "did not act": a missing name must degrade to the
// old behaviour, never to a callback that is silently never run again.
static unsigned g_vanilla_acted_tick = ~0u;   // tick on which vanilla already attacked / used
static bool vanilla_acted_this_tick(JNIEnv* env, jobject mc, jobject player, bool pre_tick)
{
    bool acted = false;
    if (mc && g_cache.mc_item_use_cooldown_fid) {
        const jint cd = env->GetIntField(mc, g_cache.mc_item_use_cooldown_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else acted = (cd == 4);
    }
    if (!acted && player && g_cache.player_last_attacked_ticks_fid) {
        const jint t = env->GetIntField(player, g_cache.player_last_attacked_ticks_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else acted = (t == (pre_tick ? 0 : 1));
    }
    return acted;
}

// Has this exact pair already been carried by a flying? Bit equality on
// purpose: the applied value is written into Entity.yaw/pitch as is, the
// original copies getYaw()/getPitch() into lastYaw/lastPitch as is, and the
// tracker reads those back -- the same float end to end.
static bool wire_settled_for(float yaw, float pitch)
{
    return g_wire_have && yaw == g_wire_yaw && pitch == g_wire_pitch;
}

static ActionResult dispatch_on_ready(JNIEnv* env, jobject mc, jobject player,
                                      bool position_final, bool frame_open)
{
    ActionRegistration* r = find_registration(g_action_owner);
    if (!r || !r->cb.on_ready) return ActionResult::Wait;
    if (!on_hook_thread()) return ActionResult::Wait;
    if (env->PushLocalFrame(64) < 0) { env->ExceptionClear(); return ActionResult::Wait; }

    // applied_* is the published value, which open_movement_frame (pre-tick)
    // or the swap-in below (fallback) has ALREADY written into the fields,
    // and nothing re-steps or re-snaps it before the original reads it.
    // after_entity_actions coincides with position_final: the fallback point
    // is the one that sits behind this tick's PlayerInput / EntityActions.
    const float applied_yaw   = g_rot_yaw.load(std::memory_order_relaxed);
    const float applied_pitch = g_rot_pitch.load(std::memory_order_relaxed);
    ActionContext ctx{ env, mc, player, applied_yaw, applied_pitch,
                       position_final, frame_open, position_final, g_tick_id,
                       g_wire_yaw, g_wire_pitch, g_wire_prev_yaw, g_wire_prev_pitch,
                       g_wire_have_prev, wire_settled_for(applied_yaw, applied_pitch), g_wire_tick };
    ActionResult res = ActionResult::Wait;
    g_in_action_dispatch = true;
    try { res = r->cb.on_ready(ctx); }
    catch (const std::exception& ex) {
        VLOG_WARN("silentaim", "action callback failed: {}", ex.what());
        res = ActionResult::Abort;
    }
    catch (...) { res = ActionResult::Abort; }
    g_in_action_dispatch = false;
    if (env->ExceptionCheck()) { env->ExceptionClear(); res = ActionResult::Abort; }
    env->PopLocalFrame(nullptr);

    if (g_action_release_pending) { g_action_release_pending = false; res = ActionResult::Abort; }
    return res;
}

static void dispatch_on_tick(JNIEnv* env, jobject mc, jobject player, bool position_final)
{
    if (!on_hook_thread()) return;
    if (!g_cache.entity_yaw_fid || !g_cache.entity_pitch_fid) return;
    for (auto& r : g_action_regs) {
        if (!r.owner || !r.cb.on_tick) continue;
        if (env->PushLocalFrame(64) < 0) { env->ExceptionClear(); continue; }
        const float real_yaw   = env->GetFloatField(player, g_cache.entity_yaw_fid);
        const float real_pitch = env->GetFloatField(player, g_cache.entity_pitch_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        ActionContext ctx{ env, mc, player, real_yaw, real_pitch,
                           position_final, false, position_final, g_tick_id,
                           g_wire_yaw, g_wire_pitch, g_wire_prev_yaw, g_wire_prev_pitch,
                           g_wire_have_prev, wire_settled_for(real_yaw, real_pitch), g_wire_tick };
        g_in_action_dispatch = true;
        try { r.cb.on_tick(ctx); }
        catch (const std::exception& ex) { VLOG_WARN("silentaim", "tick callback failed: {}", ex.what()); }
        catch (...) {}
        g_in_action_dispatch = false;
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }
    if (g_action_release_pending) { g_action_release_pending = false; begin_action_release(); }
}

// Applies an on_ready verdict at either invocation point.
static void settle_action_result(ActionResult res)
{
    g_action_ran_tick = g_tick_id;
    switch (res) {
    case ActionResult::Done:
        // Keep the owner and the applied angle until this tick's flying has
        // gone out -- Grim judges the interaction against THAT rotation --
        // and make sure there IS one (the forced flying in the hook).
        g_action_release_after_send = true;
        g_action_done_tick = g_tick_id;
        break;
    case ActionResult::Abort:
        begin_action_release();
        break;
    case ActionResult::Wait:
    default:
        break;
    }
}

// How far the sent rotation may sit from the player's own view and still make
// an attack defensible. An entity 0.6 blocks wide subtends about 11 degrees at
// arm's length, so a handful of degrees still lands on whatever they clicked.
static constexpr float kMaxJustifiableOffset = 6.0f;

bool attack_would_be_unjustified()
{
    return rotation_spoof_active()
        && g_rot_offset.load(std::memory_order_relaxed) > kMaxJustifiableOffset;
}

// â”€â”€ the rotation hook â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static jmethodID  ORIG_sendMovementPackets = nullptr;
static jmethodID  g_smp_method_id          = nullptr;
static bool       g_hook_attached          = false;
static bool       g_probe_passed           = false;

// Counts hook entries. The probe reads it to tell "the detour never fired"
// (attach silently did not take) apart from "it fired but the original inside
// it did nothing" -- two different failures that both end in no spoofing.
static std::atomic<int> g_hook_calls{ 0 };

bool silent_rotation_available() { return g_hook_attached && g_probe_passed; }

bool rotation_spoof_active()
{
    return g_hook_attached && g_probe_passed
        && g_rot_active.load(std::memory_order_acquire);
}


// hkSendMovementPackets -- the whole point of the module.
//
// Apply the managed rotation, run the original (which reads getYaw()/getPitch()
// while building the move packet), restore. The spoofed value exists only for
// the duration of this call: the camera renders later in the frame, off the
// restored fields, so nothing visible ever moves.
//
// prevYaw/prevPitch are deliberately left alone. sendMovementPackets does not
// read them, and writing them would feed the render interpolation -- which is
// precisely the leak that turns a silent aim into a visible one.
//
// AimDuplicateLook -- why nothing here decides whether a rotation goes out.
// The original computes (getYaw() - lastYawClient, getPitch() - lastPitchClient)
// in float, sends a Look/Full only when either difference is non-zero, and
// only then copies getYaw()/getPitch() into lastYawClient/lastPitchClient
// (class_746.method_3136 on 1.21.11: field_3941 / field_3925). Float
// subtraction of two finite values is zero exactly when they are equal, so a
// packet carrying the SAME pair as the previous rotation packet cannot come
// out of that code -- unless something writes those two fields. Nothing in
// this module does: lastYaw/lastPitch are only ever READ (the wire tracker
// below); this file writes Entity.yaw/pitch, the prev* pair,
// renderYaw/renderPitch, the body/head yaw and the key states, and every one
// of those is restored after the original returns, i.e. after the comparison
// has already happened. A spoof that holds the same value tick after tick
// (target == real, the hold) therefore sends exactly what vanilla would:
// nothing, until it changes.
//
// The ONE field this hook writes is ticksSinceLastPositionPacketSent
// (field_3923), and only on a tick where an action's packets went out and
// vanilla would otherwise send no flying at all (rotation unchanged, no
// movement): it is set to 20 so the original sends the PositionAndOnGround
// it sends every twentieth stationary tick anyway -- position unchanged,
// rotation NOT included, so no duplicate look. Grim's post-flying checks
// (RotationPlace, Reach, HitboxPlace) evaluate the action against the flying
// that follows it; without this one they would wait for a later flying
// carrying whatever the lease had moved to by then. See the ActionContext
// comment in the header for the resulting cadence.
// Opened by the handleInputEvents hook at the front of the tick, closed here at
// the back of it. See the movement-correction section below.
static bool g_frame_open;
static void close_movement_frame(JNIEnv* env, jobject player);

// One read of vanilla's send bookkeeping. ok is false when the fields did
// not resolve (the tracker then never reports a settled wire, which makes
// every block click wait -- loud in the log, never a wrong packet).
struct WireSnap { jfloat last_yaw = 0.f, last_pitch = 0.f; jint ticks = 0; bool ok = false; };
static WireSnap read_wire(JNIEnv* env, jobject player)
{
    WireSnap s;
    if (!g_cache.player_last_yaw_fid || !g_cache.player_last_pitch_fid || !g_cache.player_ticks_since_pos_fid) {
        if (!g_wire_fields_warned) {
            g_wire_fields_warned = true;
            VLOG_WARN("silentaim", "wire tracker: lastYaw/lastPitch/ticksSinceLastPositionPacketSent unresolved "
                                  "-- block clicks cannot settle on the wire");
        }
        return s;
    }
    s.last_yaw   = env->GetFloatField(player, g_cache.player_last_yaw_fid);
    s.last_pitch = env->GetFloatField(player, g_cache.player_last_pitch_fid);
    s.ticks      = env->GetIntField(player, g_cache.player_ticks_since_pos_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return s; }
    s.ok = true;
    return s;
}

static void JNICALL hkSendMovementPackets(JNIEnv* env, jobject thiz)
{
    vanta::NativeCallback callbackLifetime;
    if (!ORIG_sendMovementPackets || !g_cache.client_player_class) return;

    g_hook_calls.fetch_add(1, std::memory_order_release);

    const bool shutting = g_shutting_down.load(std::memory_order_acquire);

    // Sprint/sneak as they stand on this call. EVERY dispatch from this
    // function -- the fallback on_tick below as much as the fallback on_ready
    // -- sits AFTER this tick's PlayerInput and sprint/sneak EntityActions, so
    // a slot switch, a slot restore or an interaction on a tick where either
    // toggled is PacketOrderE; those ticks are skipped outright. Read first,
    // because the on_tick dispatch used to run above this and could restore
    // the slot behind a sprint toggle.
    static jboolean last_sprint = JNI_FALSE, last_sneak = JNI_FALSE;
    bool toggled = false;
    if (!shutting && g_cache.entity_is_sprinting_mid && g_cache.entity_is_sneaking_mid) {
        const jboolean sp = env->CallBooleanMethod(thiz, g_cache.entity_is_sprinting_mid);
        const jboolean sn = env->CallBooleanMethod(thiz, g_cache.entity_is_sneaking_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else { toggled = (sp != last_sprint) || (sn != last_sneak); last_sprint = sp; last_sneak = sn; }
    }

    // Fallback tick bookkeeping. When the pre-tick anchor never arrives (the
    // triggerbot's handleBlockBreaking detour is not attached on this host)
    // this is the only once-per-tick point there is, so the tick counter,
    // the lease expiry, the rotation step and the on_tick dispatch all live
    // here. The fields still hold the REAL rotation at this point.
    jobject fallback_mc = nullptr;
    if (!shutting && g_probe_passed && !g_pretick_live.load(std::memory_order_acquire)) {
        ++g_tick_id;
        if (g_action_owner && !action_live()) begin_action_release();
        if (g_cache.entity_yaw_fid && g_cache.entity_pitch_fid) {
            const jfloat ry = env->GetFloatField(thiz, g_cache.entity_yaw_fid);
            const jfloat rp = env->GetFloatField(thiz, g_cache.entity_pitch_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else {
                refresh_mouse_lattice(env, nullptr);
                fallback_mc = fetch_minecraft(env);
                if (fallback_mc && vanilla_acted_this_tick(env, fallback_mc, thiz, /*pre_tick=*/false))
                    g_vanilla_acted_tick = g_tick_id;
                if (fallback_mc && !toggled && g_vanilla_acted_tick != g_tick_id)
                    dispatch_on_tick(env, fallback_mc, thiz, true);
                step_action_rotation(ry, rp);
            }
        }
    }

    const bool spoof    = !shutting
                       && g_probe_passed
                       && g_rot_active.load(std::memory_order_acquire)
                       && g_cache.entity_yaw_fid
                       && g_cache.entity_pitch_fid;

    // Two ways the spoofed rotation can be in place by the time we get here.
    //
    //   frame_open  -- handleInputEvents already opened it at the front of the
    //                  tick, so travel() has ALREADY run in the spoofed basis
    //                  with WASD remapped. The fields are correct as they
    //                  stand; touching them again would double-apply. We only
    //                  have to close the frame after the packet goes out.
    //
    //   otherwise   -- movement correction is unavailable on this host, so the
    //                  spoof is local to this one call: swap in, send, swap
    //                  back. The rotation reaches the server but the client
    //                  moved in the real basis, which is what Simulation sees.
    const bool frame_open = g_frame_open;

    jfloat saved_yaw   = 0.f;
    jfloat saved_pitch = 0.f;

    if (spoof && !frame_open) {
        saved_yaw   = env->GetFloatField(thiz, g_cache.entity_yaw_fid);
        saved_pitch = env->GetFloatField(thiz, g_cache.entity_pitch_fid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else {
            env->SetFloatField(thiz, g_cache.entity_yaw_fid,
                               g_rot_yaw.load(std::memory_order_relaxed));
            env->SetFloatField(thiz, g_cache.entity_pitch_fid,
                               g_rot_pitch.load(std::memory_order_relaxed));
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // FALLBACK action point: the pre-tick anchor did not run on_ready this
    // tick (no anchor, or the movement frame could not open). The fields now
    // hold the applied rotation and the original below sends the flying with
    // it, which is the order Grim's RotationPlace / Reach evaluate against.
    // Residual: this sits after PlayerInput and the sprint packet, hence the
    // toggle guard; and never behind a vanilla attack / use of this tick.
    if (spoof && action_live() && g_action_ran_tick != g_tick_id && !toggled
        && g_vanilla_acted_tick != g_tick_id) {
        jobject mc = fallback_mc ? fallback_mc : fetch_minecraft(env);
        if (mc) {
            settle_action_result(dispatch_on_ready(env, mc, thiz, true, false));
            if (!fallback_mc) env->DeleteLocalRef(mc);
        }
    }
    if (fallback_mc) { env->DeleteLocalRef(fallback_mc); fallback_mc = nullptr; }

    /* @the camera goes home for exactly this call.

       sendMovementPackets is one big `if (!isCamera()) return;`, so while
       Spectate has the camera parked in another player's head the original
       below sends the server nothing at all — and Grim's BadPacketsR flags the
       silence within two seconds. Spectate's own detour on isCamera cannot help
       here: the original we are about to call lives in JNIHook's renamed COPY
       of the class, and the copy's isCamera call resolves to the copy's own
       pristine method rather than to the real class's native override.

       So the camera itself is lent back for the duration. No-op unless
       spectating. Unconditionally returned afterwards, including when the
       original throws. */
    // The wire tracker: vanilla's bookkeeping as it stands before the
    // original, so what changed across the call tells what went out.
    WireSnap before = read_wire(env, thiz);

    // Preserve vanilla movement cadence. Interactions do not imply a change
    // in position; forcing the position counter emits duplicate coordinates.

    ::vanta::modules::spectate::borrow_camera(env);

    env->CallNonvirtualVoidMethod(thiz, g_cache.client_player_class,
                                  ORIG_sendMovementPackets);
    if (env->ExceptionCheck()) env->ExceptionClear();

    ::vanta::modules::spectate::return_camera(env);

    // What went out. lastYaw/lastPitch are copied from the fields exactly
    // when a look was sent (so any change IS a look; the fields still hold
    // the applied rotation here, nothing has restored them yet); the counter
    // is zeroed exactly when a position was sent.
    if (before.ok) {
        const WireSnap after = read_wire(env, thiz);
        if (after.ok) {
            const bool look_sent = after.last_yaw != before.last_yaw || after.last_pitch != before.last_pitch;
            const bool pos_sent  = after.ticks == 0;
            if (look_sent) {
                g_wire_prev_yaw = g_wire_yaw; g_wire_prev_pitch = g_wire_pitch;
                g_wire_have_prev = g_wire_have;
                g_wire_yaw = after.last_yaw; g_wire_pitch = after.last_pitch;
                g_wire_have = true;
            }
            if (look_sent || pos_sent) g_wire_tick = g_tick_id;
        }
    }

    if (frame_open) {
        // The packet is out and the movement is done -- hand the player back
        // their real rotation and keys before anything renders.
        close_movement_frame(env, thiz);
    } else if (spoof) {
        env->SetFloatField(thiz, g_cache.entity_yaw_fid,   saved_yaw);
        env->SetFloatField(thiz, g_cache.entity_pitch_fid, saved_pitch);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (spoof && (!action_spoofing() || g_action_model)) {

        // Turn the MODEL here rather than from the render pump, and the third
        // person view stops shaking.
        //
        // The pump runs at swapBuffers, i.e. AFTER the frame is drawn. Vanilla
        // recomputes bodyYaw inside the tick, so a frame that ticked rendered
        // vanilla's angle and a frame that did not rendered ours -- the two
        // alternating at 20 Hz is exactly the vibration. This point is inside
        // the tick and after LivingEntity has already done its body-yaw work
        // (tick() runs super.tick() before sendMovementPackets), so our value
        // is the last writer before the frame is drawn and nothing competes
        // with it.
        //
        // These are render-only fields -- the camera reads Entity.yaw/pitch,
        // which we have just restored -- so this shows the rotation without
        // moving the view and without adding anything to the wire.
        // ONLY the live fields. The prev* pair is deliberately not touched.
        //
        // Writing it was both unnecessary and actively harmful. Unnecessary
        // because LivingEntity.tick() already does prevBodyYaw = bodyYaw at the
        // top of every tick, so by the time we get here prev already holds LAST
        // tick's aim angle and the render interpolates aim -> aim, which is
        // exactly right. Harmful because the prev* intermediary names were a
        // guess, and their neighbours in LivingEntity are handSwingProgress and
        // the limb-animation floats: a stray write of a yaw value (hundreds of
        // degrees) into one of those is what threw the held item off screen and
        // made the head judder back and forth.
        const jfloat yaw = g_rot_yaw.load(std::memory_order_relaxed);
        if (g_cache.living_body_yaw_fid)
            env->SetFloatField(thiz, g_cache.living_body_yaw_fid, yaw);
        if (g_cache.living_head_yaw_fid)
            env->SetFloatField(thiz, g_cache.living_head_yaw_fid, yaw);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // The flying that carries the verified rotation is out: the lease may go.
    // The pump re-leases within the frame if it still has a target, and the
    // step then continues from the current angle; otherwise the easing walks
    // it home.
    if (g_action_release_after_send) begin_action_release();
}

// One watched primitive field and its value at snapshot time. Declared up here
// because both probes use it -- see snapshot_fields() below for why the
// verification works this way at all.
struct FieldSnap {
    jfieldID fid;
    char     kind;   // 'I', 'F' or 'D'
    jint     i = 0;
    jfloat   f = 0.f;
    jdouble  d = 0.0;
};
static bool snapshot_fields(JNIEnv* env, jclass cls, jobject obj,
                            std::vector<FieldSnap>& out, bool resolve);

// â”€â”€ movement correction â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Why Simulation fires without this.
//
// LivingEntity.travel() turns the WASD input into world motion by rotating it
// through the player's yaw. The client does that with the real yaw, because
// that is what is in the field while the tick runs. The server then replays the
// same tick using the yaw off the wire -- the spoofed one -- and lands somewhere
// else. That difference IS the Simulation offset; it grows with speed, which is
// why sprinting and jumping make it worse.
//
// The fix is the one Vape uses: make the client move with the SPOOFED yaw too,
// and remap WASD so the world-space direction the player asked for is
// preserved. Client and server then agree exactly, whatever Grim predicts.
//
// The cost is that the remap is quantised to the eight directions WASD can
// express, so the player's actual heading can be off by up to ~22 degrees from
// what they intended. That drift is a control nuisance, not a flag: both sides
// still compute the same thing. Vape ships the same tradeoff.

// The pre-movement anchor is MinecraftClient.handleInputEvents -- PRIVATE, and
// that is the whole reason it is this method and not tickMovement.
//
// Measured on this host: hooking ClientPlayerEntity.tickMovement attaches fine
// and then silently does nothing, while sendMovementPackets on the SAME class
// works. The difference is visibility. tickMovement is a public override, and
// CallNonvirtual* on the copy class for an overridden method re-enters the
// detour, where JNIHook's recursion guard turns it into a no-op. Private
// methods have no override to re-enter, so they run.
//
// handleInputEvents sits in MinecraftClient.tick() ahead of world.tickEntities(),
// so it brackets the player tick from the front while sendMovementPackets --
// already hooked, already private, already verified -- closes it from the back.
// Apply there, restore here, and the spoofed frame covers exactly travel().
static jmethodID ORIG_handleInputEvents = nullptr;
static jmethodID g_hie_method_id        = nullptr;
static bool      g_hie_attached         = false;
static std::atomic<bool> g_hie_verified { false };
static std::atomic<bool> g_hie_broken   { false };

// Set while the player is inside a spoofed movement frame: yaw/pitch and the
// movement keys have been swapped and MUST be put back. Declared above, next
// to the hook that closes it.
// renderYaw/renderPitch as they stood BEFORE the tick, i.e. the values vanilla
// will have copied into lastRenderYaw/lastRenderPitch. Keeping them lets the
// close recompute what the smoothed view SHOULD have become, chasing the real
// angle instead of the spoofed one.
static jfloat g_frame_render_yaw   = 0.f;
static jfloat g_frame_render_pitch = 0.f;
static int    g_render_fix_state   = 0;   // 0 untested, 1 confirmed, -1 rejected

static jfloat g_frame_saved_yaw   = 0.f;
static jfloat g_frame_saved_pitch = 0.f;
static jboolean g_frame_saved_keys[4] = { JNI_FALSE, JNI_FALSE, JNI_FALSE, JNI_FALSE };
static bool   g_frame_keys_written = false;

// g_pretick_live (declared with the lease state above) is set the first time
// the triggerbot's pre-tick callback reaches us. There is nothing to attach and
// nothing to verify: that detour is already installed and proven in
// production, so its arrival IS the confirmation.

bool movement_correction_available()
{
    return g_pretick_live.load(std::memory_order_acquire);
}

// KeyBinding.pressed is the only boolean field the class declares, so it can be
// found by shape instead of by a name that moves between versions.
static jfieldID find_sole_boolean_field(JNIEnv* env, jclass cls)
{
    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!class_cls || !field_cls) return nullptr;

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    jmethodID get_type   = env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;");
    jmethodID get_mods   = env->GetMethodID(field_cls, "getModifiers", "()I");
    jmethodID type_name  = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!get_fields || !get_name || !get_type || !get_mods || !type_name) return nullptr;

    jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(cls, get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!arr) return nullptr;

    jfieldID found = nullptr;
    int      hits  = 0;
    const jsize n = env->GetArrayLength(arr);
    for (jsize k = 0; k < n; ++k) {
        jobject fld = env->GetObjectArrayElement(arr, k);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!fld) continue;

        const jint mods = env->CallIntMethod(fld, get_mods);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        if (mods & 0x0008) { env->DeleteLocalRef(fld); continue; }

        jobject ty = env->CallObjectMethod(fld, get_type);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        if (!ty) { env->DeleteLocalRef(fld); continue; }

        jstring tname = static_cast<jstring>(env->CallObjectMethod(ty, type_name));
        env->DeleteLocalRef(ty);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        bool is_bool = false;
        if (tname) {
            const char* tn = env->GetStringUTFChars(tname, nullptr);
            if (tn) { is_bool = (std::strcmp(tn, "boolean") == 0);
                      env->ReleaseStringUTFChars(tname, tn); }
            env->DeleteLocalRef(tname);
        }
        if (!is_bool) { env->DeleteLocalRef(fld); continue; }

        jstring jname = static_cast<jstring>(env->CallObjectMethod(fld, get_name));
        env->DeleteLocalRef(fld);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!jname) continue;

        const char* nm = env->GetStringUTFChars(jname, nullptr);
        if (nm) {
            jfieldID fid = env->GetFieldID(cls, nm, "Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
            else if (fid) { found = fid; ++hits; }
            env->ReleaseStringUTFChars(jname, nm);
        }
        env->DeleteLocalRef(jname);
    }
    env->DeleteLocalRef(arr);

    // Exactly one, or we have not identified it and must not guess.
    return (hits == 1) ? found : nullptr;
}

// Finds forward / back / left / right on GameOptions by their translation key,
// which is stable across every version and every mapping flavour.
static void resolve_movement_keys(JNIEnv* env, jobject mc)
{
    if (g_cache.move_keys_resolved) return;
    if (!g_cache.mc_options_fid || !g_cache.key_binding_class ||
        !g_cache.key_binding_translation_mid) return;

    jobject options = env->GetObjectField(mc, g_cache.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;

    jclass opts_cls  = env->GetObjectClass(options);
    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

    jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(opts_cls, get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }
    if (!arr) { env->DeleteLocalRef(options); return; }

    static const char* kWanted[4] = { "key.forward", "key.back", "key.left", "key.right" };
    int resolved = 0;

    const jsize n = env->GetArrayLength(arr);
    for (jsize k = 0; k < n; ++k) {
        jobject fld = env->GetObjectArrayElement(arr, k);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!fld) continue;

        jstring jname = static_cast<jstring>(env->CallObjectMethod(fld, get_name));
        env->DeleteLocalRef(fld);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!jname) continue;

        const char* nm = env->GetStringUTFChars(jname, nullptr);
        jfieldID fid = nullptr;
        if (nm) {
            fid = env->GetFieldID(opts_cls, nm, "Lnet/minecraft/class_304;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->ReleaseStringUTFChars(jname, nm);
        }
        env->DeleteLocalRef(jname);
        if (!fid) continue;

        jobject kb = env->GetObjectField(options, fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;

        jstring tk = static_cast<jstring>(
            env->CallObjectMethod(kb, g_cache.key_binding_translation_mid));
        env->DeleteLocalRef(kb);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!tk) continue;

        const char* t = env->GetStringUTFChars(tk, nullptr);
        if (t) {
            for (int w = 0; w < 4; ++w) {
                if (std::strcmp(t, kWanted[w]) == 0 && !g_cache.move_key_fids[w]) {
                    g_cache.move_key_fids[w] = fid;
                    ++resolved;
                }
            }
            env->ReleaseStringUTFChars(tk, t);
        }
        env->DeleteLocalRef(tk);
    }
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(options);

    const bool all = g_cache.move_key_fids[0] && g_cache.move_key_fids[1]
                  && g_cache.move_key_fids[2] && g_cache.move_key_fids[3];
    if (all) {
        g_cache.move_keys_resolved = true;
        VLOG_INFO("silentaim", "movement keys resolved by translation key ({} matched)", resolved);
    }
}

static jobject fetch_minecraft(JNIEnv* env)
{
    if (!g_cache.minecraft_class) return nullptr;
    jfieldID inst = env->GetStaticFieldID(g_cache.minecraft_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!inst) return nullptr;
    jobject mc = env->GetStaticObjectField(g_cache.minecraft_class, inst);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return mc;
}

static jobject fetch_options(JNIEnv* env)
{
    if (!g_cache.mc_options_fid) return nullptr;
    jobject mc = fetch_minecraft(env);
    if (!mc) return nullptr;
    jobject opts = env->GetObjectField(mc, g_cache.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); opts = nullptr; }
    env->DeleteLocalRef(mc);
    return opts;
}

// Opens the spoofed movement frame: the player's yaw becomes the aim yaw, and
// WASD is remapped so the world-space heading they asked for is preserved.
static void open_movement_frame(JNIEnv* env, jobject player)
{
    if (g_frame_open) return;
    if (!g_cache.entity_yaw_fid || !g_cache.entity_pitch_fid
        || !g_cache.key_binding_pressed_fid || !g_cache.move_keys_resolved) return;

    g_frame_saved_yaw   = env->GetFloatField(player, g_cache.entity_yaw_fid);
    g_frame_saved_pitch = env->GetFloatField(player, g_cache.entity_pitch_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // Snapshot the smoothed view before the tick touches it.
    if (g_cache.player_render_yaw_fid && g_render_fix_state >= 0) {
        g_frame_render_yaw   = env->GetFloatField(player, g_cache.player_render_yaw_fid);
        g_frame_render_pitch = g_cache.player_render_pitch_fid
            ? env->GetFloatField(player, g_cache.player_render_pitch_fid) : 0.f;
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    jobject options = fetch_options(env);
    if (!options) return;

    jobject kb[4] = { nullptr, nullptr, nullptr, nullptr };
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        kb[i] = env->GetObjectField(options, g_cache.move_key_fids[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
        if (!kb[i]) ok = false;
    }

    g_frame_keys_written = false;
    if (ok) {
        for (int i = 0; i < 4; ++i)
            g_frame_saved_keys[i] = env->GetBooleanField(kb[i], g_cache.key_binding_pressed_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();

        // Input as the player actually holds it. Sideways is LEFT-positive,
        // matching vanilla's KeyboardInput.
        const float fwd  = (g_frame_saved_keys[0] == JNI_TRUE ? 1.f : 0.f)
                         - (g_frame_saved_keys[1] == JNI_TRUE ? 1.f : 0.f);
        const float side = (g_frame_saved_keys[2] == JNI_TRUE ? 1.f : 0.f)
                         - (g_frame_saved_keys[3] == JNI_TRUE ? 1.f : 0.f);

        if (fwd != 0.f || side != 0.f) {
            constexpr double kPi = 3.14159265358979323846;
            const float managed = g_rot_yaw.load(std::memory_order_relaxed);

            // travel() produces world heading = yaw - atan2(side, fwd). Invert
            // that against the spoofed yaw to get the input which reproduces
            // the SAME heading from the new basis.
            const double intended = std::atan2(static_cast<double>(side),
                                               static_cast<double>(fwd)) * 180.0 / kPi;
            const double travel_yaw = static_cast<double>(g_frame_saved_yaw) - intended;

            double rel = static_cast<double>(managed) - travel_yaw;
            rel = std::fmod(rel + 180.0, 360.0);
            if (rel < 0.0) rel += 360.0;
            rel -= 180.0;

            const double rad = rel * kPi / 180.0;
            const double f2  = std::cos(rad);
            const double s2  = std::sin(rad);

            // 0.4 is Vape's DEFAULT_MOVEMENT_THRESHOLD: the point at which a
            // diagonal is worth pressing rather than rounding to the nearer
            // cardinal.
            constexpr double kThresh = 0.4;
            const jboolean want[4] = {
                f2 >=  kThresh ? JNI_TRUE : JNI_FALSE,   // forward
                f2 <= -kThresh ? JNI_TRUE : JNI_FALSE,   // back
                s2 >=  kThresh ? JNI_TRUE : JNI_FALSE,   // left
                s2 <= -kThresh ? JNI_TRUE : JNI_FALSE,   // right
            };
            for (int i = 0; i < 4; ++i)
                env->SetBooleanField(kb[i], g_cache.key_binding_pressed_fid, want[i]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            g_frame_keys_written = true;
        }

        env->SetFloatField(player, g_cache.entity_yaw_fid,
                           g_rot_yaw.load(std::memory_order_relaxed));
        env->SetFloatField(player, g_cache.entity_pitch_fid,
                           g_rot_pitch.load(std::memory_order_relaxed));
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_frame_open = true;
    }

    for (int i = 0; i < 4; ++i) if (kb[i]) env->DeleteLocalRef(kb[i]);
    env->DeleteLocalRef(options);
}

// Closes it. MUST run before anything renders, or the camera shows the spoof.
static void close_movement_frame(JNIEnv* env, jobject player)
{
    if (!g_frame_open) return;
    g_frame_open = false;

    if (g_cache.entity_yaw_fid && g_cache.entity_pitch_fid) {
        // prevYaw/prevPitch have to come back too, and this is the whole
        // reason the held item was flying off screen.
        //
        // The frame opens BEFORE world.tickEntities(), and the first thing
        // Entity.tick() does is prevYaw = getYaw(). So vanilla photographs the
        // SPOOFED angle into prev. Restoring only the live field then leaves
        // the renderer interpolating lerp(tickDelta, prev = spoofed, cur =
        // real) on every single frame -- a per-frame oscillation between the
        // aim and the player's view. That is the head juddering back and forth
        // and the first-person item swinging out of frame, and it scales with
        // how far the aim sits from where they are looking.
        //
        // Vanilla's own value for prev at this point is the real angle the tick
        // started with, which is exactly what was saved on open.
        //
        // The field NAME is not trusted -- two earlier guesses in this file
        // were wrong. Instead it is confirmed by behaviour: if the candidate
        // currently holds the spoofed angle, then vanilla just assigned it from
        // yaw this tick, so it IS prev. Anything else and nothing is written.
        auto restore_prev = [&](jfieldID fid, jfloat spoofed, jfloat real) {
            if (!fid) return;
            const jfloat now = env->GetFloatField(player, fid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
            if (std::fabs(now - spoofed) < 0.001f)
                env->SetFloatField(player, fid, real);
        };
        restore_prev(g_cache.entity_prev_yaw_fid,
                     g_rot_yaw.load(std::memory_order_relaxed),   g_frame_saved_yaw);
        restore_prev(g_cache.entity_prev_pitch_fid,
                     g_rot_pitch.load(std::memory_order_relaxed), g_frame_saved_pitch);

        env->SetFloatField(player, g_cache.entity_yaw_fid,   g_frame_saved_yaw);
        env->SetFloatField(player, g_cache.entity_pitch_fid, g_frame_saved_pitch);
        if (env->ExceptionCheck()) env->ExceptionClear();

        // And the smoothed view the held item is measured against.
        //
        // HeldItemRenderer sways the first-person item by
        //     (getYaw(td) - lerp(td, lastRenderYaw, renderYaw)) * 0.1
        // and ClientPlayerEntity advances renderYaw halfway toward getYaw()
        // from inside the tick -- so it chased the SPOOFED angle. Restoring
        // only the live yaw leaves the renderer comparing the real view against
        // a smoothed copy of the aim, and the sword swings out of frame by an
        // amount proportional to how far the aim sits from the crosshair.
        //
        // Recomputed here as vanilla would have: halfway from its pre-tick
        // value toward the REAL angle. lastRenderYaw needs no fixing because
        // vanilla copies it from renderYaw, which this keeps correct.
        //
        // Identity is confirmed by behaviour before anything is written: the
        // field must currently hold exactly the halfway point between its
        // pre-tick value and the spoofed angle. Nothing else in the class moves
        // that way, so a wrong field can never be written to.
        if (g_cache.player_render_yaw_fid && g_render_fix_state >= 0) {
            const jfloat spoofed = g_rot_yaw.load(std::memory_order_relaxed);
            const jfloat expect  = g_frame_render_yaw + (spoofed - g_frame_render_yaw) * 0.5f;
            const jfloat actual  = env->GetFloatField(player, g_cache.player_render_yaw_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();

            if (g_render_fix_state == 0) {
                g_render_fix_state = (std::fabs(actual - expect) < 0.05f) ? 1 : -1;
                // Plain {} only -- this logger does not implement format specs,
                // and "{:.3f}" was being emitted literally, which swallowed the
                // verdict and made the line useless.
                VLOG_INFO("silentaim",
                          "held-item sway fix: expected={} actual={} -> {}",
                          expect, actual,
                          g_render_fix_state == 1 ? "CONFIRMED"
                                                  : "rejected (field does not behave like renderYaw)");
            }

            if (g_render_fix_state == 1) {
                env->SetFloatField(player, g_cache.player_render_yaw_fid,
                    g_frame_render_yaw + (g_frame_saved_yaw - g_frame_render_yaw) * 0.5f);
                if (g_cache.player_render_pitch_fid)
                    env->SetFloatField(player, g_cache.player_render_pitch_fid,
                        g_frame_render_pitch + (g_frame_saved_pitch - g_frame_render_pitch) * 0.5f);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }

    if (g_frame_keys_written) {
        g_frame_keys_written = false;
        jobject options = fetch_options(env);
        if (options) {
            for (int i = 0; i < 4; ++i) {
                jobject k = env->GetObjectField(options, g_cache.move_key_fids[i]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                if (!k) continue;
                env->SetBooleanField(k, g_cache.key_binding_pressed_fid, g_frame_saved_keys[i]);
                env->DeleteLocalRef(k);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(options);
        }
    }
}

// Opens the spoofed frame at the FRONT of the tick. Called by the triggerbot's
// handleBlockBreaking detour -- see the header for why that hook and not one of
// our own.
void on_pre_player_tick(JNIEnv* env, jobject minecraft)
{
    if (!env || !minecraft) return;
    if (g_shutting_down.load(std::memory_order_acquire)) return;
    if (!g_cache.initialized || !g_cache.mc_player_fid) return;

    if (!g_pretick_live.exchange(true, std::memory_order_acq_rel))
        VLOG_INFO("silentaim", "pre-tick anchor live -- movement correction is on");

    // PRIMARY action point. This runs inside handleInputEvents, where vanilla
    // itself attacks and uses items: before PlayerInput, before the sprint
    // EntityAction, before travel(), before the flying. Order of business:
    //   1. tick id, lease expiry
    //   2. on_tick for every registered owner -- fields hold the REAL rotation
    //   3. one step of the action rotation, snapped
    //   4. open the movement frame (fields now hold the APPLIED rotation)
    //   5. on_ready exactly once per tick
    ++g_tick_id;
    if (g_action_owner && !action_live()) begin_action_release();

    bool any_tick_cb = false;
    for (const auto& r : g_action_regs) if (r.owner && r.cb.on_tick) { any_tick_cb = true; break; }

    if (!g_rot_active.load(std::memory_order_acquire) && !action_spoofing() && !any_tick_cb) return;

    jobject player = env->GetObjectField(minecraft, g_cache.mc_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!player) return;

    refresh_mouse_lattice(env, minecraft);

    // doAttack / doItemUse already ran this tick (they precede this point in
    // handleInputEvents): no slot packet and no interaction of ours may follow
    // them inside the tick, so neither callback runs. The lease survives (it
    // is refreshed by the pump) and on_ready simply runs on the next tick;
    // the rotation still steps, so the approach never stalls.
    const bool vanilla_acted = vanilla_acted_this_tick(env, minecraft, player, /*pre_tick=*/true);
    if (vanilla_acted) g_vanilla_acted_tick = g_tick_id;

    if (any_tick_cb && !vanilla_acted) dispatch_on_tick(env, minecraft, player, false);

    if (action_spoofing() && g_cache.entity_yaw_fid && g_cache.entity_pitch_fid) {
        const jfloat ry = env->GetFloatField(player, g_cache.entity_yaw_fid);
        const jfloat rp = env->GetFloatField(player, g_cache.entity_pitch_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else step_action_rotation(ry, rp);
    }

    // Order matters and is fixed: publish (step + snap + clamp) -> write the
    // fields (open_movement_frame) -> on_ready with the same published value.
    // Nothing between here and the original in hkSendMovementPackets touches
    // g_rot_* or Entity.yaw/pitch again (the aura's pump and AimAssist's
    // camera write both run from the render pump, outside the tick), so the
    // angle the callback validated is the angle the flying carries.
    if (g_rot_active.load(std::memory_order_acquire)) {
        open_movement_frame(env, player);
        if (g_frame_open && action_live() && g_action_ran_tick != g_tick_id && !vanilla_acted)
            settle_action_result(dispatch_on_ready(env, minecraft, player, false, true));
    }
    env->DeleteLocalRef(player);
}

// Dead code kept out of the build: hooking the dispatcher itself throws inside
// the original. See on_pre_player_tick above.
#if 0
static void JNICALL hkHandleInputEvents(JNIEnv* env, jobject thiz)
{
    vanta::NativeCallback callbackLifetime;
    if (!ORIG_handleInputEvents || !g_cache.minecraft_class) return;

    // Verify by WATCHING a real call. Unlike sendMovementPackets there is
    // nothing here that is safe to invoke ourselves, so the first genuine call
    // IS the probe: snapshot MinecraftClient's own primitives across it. This
    // method advances key-repeat counters and the item-use cooldown every tick,
    // so a body that actually ran always moves something.
    const bool verifying = !g_hie_verified.load(std::memory_order_acquire)
                        && !g_hie_broken.load(std::memory_order_acquire);
    std::vector<FieldSnap> before;
    if (verifying)
        snapshot_fields(env, g_cache.minecraft_class, thiz, before, true);

    env->CallNonvirtualVoidMethod(thiz, g_cache.minecraft_class, ORIG_handleInputEvents);

    // An exception here is NOT cosmetic and must never be swallowed quietly.
    //
    // handleInputEvents is a dispatcher: it calls this.doAttack(),
    // this.handleBlockBreaking(), this.setScreen(). JNIHook's class copy
    // rewrites the constant pool so self-references point at the COPY, so those
    // internal calls run with a receiver whose runtime type is the real
    // class_310 -- a type mismatch that throws. The body then aborts at the
    // first such call and everything after it, attacks included, never happens.
    //
    // Clearing that silently is exactly how "movement correction works but I
    // cannot hit anything" happens. So: notice it, say it, and tear the hook
    // down. Losing the Simulation fix is a fair price for a client whose
    // keybinds work.
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (!g_hie_broken.exchange(true, std::memory_order_acq_rel)) {
            VLOG_WARN("silentaim",
                      "handleInputEvents original THREW -- the class copy cannot make this "
                      "method's internal self-calls (doAttack et al). Detaching: movement "
                      "correction goes off so input keeps working.");
        }
        return;
    }

    if (verifying && !before.empty()) {
        std::vector<FieldSnap> after = before;
        snapshot_fields(env, g_cache.minecraft_class, thiz, after, false);
        int changed = 0;
        for (size_t k = 0; k < before.size(); ++k) {
            switch (before[k].kind) {
            case 'I': if (after[k].i != before[k].i) ++changed; break;
            case 'F': if (after[k].f != before[k].f) ++changed; break;
            case 'D': if (after[k].d != before[k].d) ++changed; break;
            default: break;
            }
        }
        if (changed > 0) {
            g_hie_verified.store(true, std::memory_order_release);
            VLOG_INFO("silentaim", "handleInputEvents probe: original RUNS ({} fields moved) "
                                   "-- movement correction is live", changed);
        } else {
            g_hie_broken.store(true, std::memory_order_release);
            VLOG_WARN("silentaim", "handleInputEvents probe: original DID NOT RUN -- movement "
                                   "correction stays off, Simulation may flag while moving");
        }
    }

    if (g_shutting_down.load(std::memory_order_acquire)) return;
    if (!g_hie_verified.load(std::memory_order_acquire)) return;
    if (!g_rot_active.load(std::memory_order_acquire)) return;
    if (!g_cache.mc_player_fid) return;

    jobject player = env->GetObjectField(thiz, g_cache.mc_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!player) return;
    open_movement_frame(env, player);
    env->DeleteLocalRef(player);
}
#endif

// â”€â”€ the probe â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Why this exists, in one paragraph.
//
// JNIHook detours by retransforming the class and pointing the "original"
// jmethodID at a renamed COPY. On MC 1.21.7+ that copy has been observed to
// turn CallNonvirtual* into a silent no-op for class_746 -- the triggerbot's
// tickMovement hook was deleted for exactly this and the player froze in place
// until it was. sendMovementPackets sits on the same class, and a no-op there
// means the client stops sending movement to the server entirely: the player
// rubber-bands on the spot and eventually times out.
//
// So the hook is verified against the METHOD IT ACTUALLY IS, not a stand-in.
// Probing a different class would prove nothing here -- the failure is specific
// to how this class retransforms, and retransforming Entity (the superclass of
// every entity in the world) merely to ask a question would be its own risk.
//
// The observable is ClientPlayerEntity.ticksSinceLastPositionPacketSent: an int
// that sendMovementPackets increments on every call for the camera entity, and
// the local player is always the camera entity. Snapshot it, call the method
// once through the freshly-attached detour, read it back:
//
//   "¢ changed     â†’ the copy ran the original body. The hook is trustworthy.
//   "¢ unchanged   â†’ the 1.21.7+ no-op. Detach immediately and fall back.
//
// The one call we make ourselves costs one extra movement packet, which is
// indistinguishable from the twenty vanilla sends every second anyway. The
// spoof gate in hkSendMovementPackets keys on g_probe_passed, so the probe
// call runs with the player's real rotation and cannot be observed.
//
// If the counter field cannot be resolved at all we refuse to hook. An
// unverifiable hook on this method is exactly the thing this probe exists to
// avoid, and Client rotation is always available as the honest fallback.
// Enumerates every non-static int/float/double field DECLARED on the class and
// records its current value.
//
// This exists because naming the observable was the wrong idea twice over. The
// first attempt compared ticksSinceLastPositionPacketSent before and after,
// which is unsound: sendMovementPackets increments it and then zeroes it again
// on any tick where the position actually goes out, so a moving player lands
// back on the same number and a healthy hook was discarded. The second attempt
// fixed the comparison but still had to GUESS the field's intermediary name,
// and a wrong guess resolves to null and refuses to hook at all -- which is
// exactly the state the module has been stuck in.
//
// So: no name, no guess. Ask the JVM what fields exist, watch all of them, and
// let ANY change stand as proof the body executed. Reflection supplies only the
// names; the reads go through JNI, which ignores Java access control, so
// nothing here needs setAccessible and nothing can throw on a private field.
static bool snapshot_fields(JNIEnv* env, jclass cls, jobject obj,
                            std::vector<FieldSnap>& out, bool resolve)
{
    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!class_cls || !field_cls) return false;

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    jmethodID get_type   = env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;");
    jmethodID get_mods   = env->GetMethodID(field_cls, "getModifiers", "()I");
    jmethodID type_name  = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!get_fields || !get_name || !get_type || !get_mods || !type_name) return false;

    size_t index = 0;

    if (resolve) {
        jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(cls, get_fields));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!arr) return false;

        const jsize n = env->GetArrayLength(arr);
        for (jsize k = 0; k < n; ++k) {
            jobject fld = env->GetObjectArrayElement(arr, k);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!fld) continue;

            const jint mods = env->CallIntMethod(fld, get_mods);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
            if (mods & 0x0008) { env->DeleteLocalRef(fld); continue; }   // ACC_STATIC

            jobject ty = env->CallObjectMethod(fld, get_type);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
            if (!ty) { env->DeleteLocalRef(fld); continue; }

            jstring tname = static_cast<jstring>(env->CallObjectMethod(ty, type_name));
            env->DeleteLocalRef(ty);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
            if (!tname) { env->DeleteLocalRef(fld); continue; }

            const char* tn = env->GetStringUTFChars(tname, nullptr);
            char kind = 0;
            const char* sig = nullptr;
            if (tn) {
                if      (std::strcmp(tn, "int")    == 0) { kind = 'I'; sig = "I"; }
                else if (std::strcmp(tn, "float")  == 0) { kind = 'F'; sig = "F"; }
                else if (std::strcmp(tn, "double") == 0) { kind = 'D'; sig = "D"; }
                env->ReleaseStringUTFChars(tname, tn);
            }
            env->DeleteLocalRef(tname);

            if (!kind) { env->DeleteLocalRef(fld); continue; }

            jstring jname = static_cast<jstring>(env->CallObjectMethod(fld, get_name));
            env->DeleteLocalRef(fld);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!jname) continue;

            const char* nm = env->GetStringUTFChars(jname, nullptr);
            jfieldID fid = nullptr;
            if (nm) {
                fid = env->GetFieldID(cls, nm, sig);
                if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                env->ReleaseStringUTFChars(jname, nm);
            }
            env->DeleteLocalRef(jname);
            if (!fid) continue;

            FieldSnap s;
            s.fid  = fid;
            s.kind = kind;
            out.push_back(s);
        }
        env->DeleteLocalRef(arr);
    }

    // Read (or re-read) the values.
    for (auto& s : out) {
        switch (s.kind) {
        case 'I': s.i = env->GetIntField(obj, s.fid);    break;
        case 'F': s.f = env->GetFloatField(obj, s.fid);  break;
        case 'D': s.d = env->GetDoubleField(obj, s.fid); break;
        default: break;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    (void)index;
    return !out.empty();
}

static bool verify_hook_runs_original(JNIEnv* env, jobject local_player,
                                      jclass cls, jmethodID smp)
{
    std::vector<FieldSnap> before;
    if (!snapshot_fields(env, cls, local_player, before, /*resolve=*/true)) {
        VLOG_WARN("silentaim", "field scan found nothing to watch -- cannot verify the hook");
        return false;
    }

    env->CallVoidMethod(local_player, smp);
    if (env->ExceptionCheck()) env->ExceptionClear();

    std::vector<FieldSnap> after = before;
    snapshot_fields(env, cls, local_player, after, /*resolve=*/false);

    int changed = 0;
    for (size_t k = 0; k < before.size() && k < after.size(); ++k) {
        switch (before[k].kind) {
        case 'I': if (after[k].i != before[k].i) ++changed; break;
        case 'F': if (after[k].f != before[k].f) ++changed; break;
        case 'D': if (after[k].d != before[k].d) ++changed; break;
        default: break;
        }
    }

    const int  fired = g_hook_calls.load(std::memory_order_acquire);
    const bool ran   = changed > 0;

    VLOG_INFO("silentaim",
              "sendMovementPackets probe: hook_fired={} watched={} changed={} â†’ {}",
              fired, static_cast<int>(before.size()), changed,
              (fired > 0 && ran) ? "original RUNS (silent rotation is live)"
                                 : "original DID NOT RUN (1.21.7+ copy-class no-op)");

    return fired > 0 && ran;
}

// Attaches the hook, then immediately verifies it. Called once, from the pump,
// with a live local player in hand.
static void install_rotation_hook(JNIEnv* env, jobject local_player)
{
    static bool tried = false;
    if (tried) return;
    tried = true;

    if (!g_cache.client_player_class) {
        VLOG_WARN("silentaim", "install: class_746 not resolved -- no rotation hook");
        return;
    }
    if (!vanta::jvm::ctx().vm.load()) return;

    // Candidate list rather than a single name. The intermediary for
    // sendMovementPackets has moved across 1.21.x drops, and the one in
    // mappings.hpp is pinned to a single version -- the autototem module hit
    // exactly this with sendPacket and solved it the same way. Whichever
    // resolves first wins, and the log says which, so a future rename is one
    // line to fix instead of a silent dead module.
    static const char* kCandidates[] = {
        "method_3136",       // mappings.hpp / 1.21.10
        "method_3134",
        "method_6001",
        "sendPosition",      // Mojang
        "sendMovementPackets",
    };

    jmethodID   smp = nullptr;
    const char* smp_name = nullptr;
    for (const char* cand : kCandidates) {
        smp = cache_method(env, g_cache.client_player_class, cand, "()V");
        if (smp) { smp_name = cand; break; }
    }
    if (!smp) {
        smp = cache_method(env, g_cache.client_player_class,
                           sdk::mappings::send_movement_packets_name,
                           sdk::mappings::send_movement_packets_sig);
        smp_name = sdk::mappings::send_movement_packets_name;
    }
    if (!smp) {
        VLOG_WARN("silentaim",
                  "install: sendMovementPackets not found on class_746 -- no rotation hook. "
                  "None of the candidate names resolved.");
        return;
    }
    VLOG_INFO("silentaim", "install: sendMovementPackets resolved as '{}'", smp_name);

    jnihook_result_t init = JNIHook_Init(vanta::jvm::ctx().vm.load());
    if (init != JNIHOOK_OK && init != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        VLOG_WARN("silentaim", "install: JNIHook_Init failed ({}) -- no rotation hook",
                  static_cast<int>(init));
        return;
    }
    VLOG_INFO("silentaim", "install: JNIHook_Init ok ({})", static_cast<int>(init));

    jnihook_result_t r = JNIHOOK_ERR_JVMTI_OPERATION;
    __try {
        r = JNIHook_Attach(smp, reinterpret_cast<void*>(hkSendMovementPackets),
                           &ORIG_sendMovementPackets);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        VLOG_WARN("silentaim", "JNIHook_Attach(sendMovementPackets) raised 0x{:08x}",
                  static_cast<unsigned long>(GetExceptionCode()));
        r = JNIHOOK_ERR_JVMTI_OPERATION;
    }

    if (r != JNIHOOK_OK || !ORIG_sendMovementPackets) {
        VLOG_WARN("silentaim", "install: JNIHook_Attach failed (r={}) -- no rotation hook",
                  static_cast<int>(r));
        ORIG_sendMovementPackets = nullptr;
        return;
    }
    VLOG_INFO("silentaim", "install: attached, verifying...");

    // Attached -- but not yet trusted. g_probe_passed is still false, so the
    // hook passes straight through without spoofing while we test it.
    g_hook_calls.store(0, std::memory_order_relaxed);
    g_smp_method_id = smp;

    if (!verify_hook_runs_original(env, local_player,
                                   g_cache.client_player_class, smp)) {
        VLOG_WARN("silentaim",
                  "detaching: a no-op original here would stop movement packets "
                  "entirely. The rotation will not reach the server.");
        __try { JNIHook_Detach(smp); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        ORIG_sendMovementPackets = nullptr;
        g_smp_method_id          = nullptr;
        return;
    }

    g_cache.send_movement_packets_mid = smp;
    g_hook_attached = true;
    g_probe_passed  = true;
    VLOG_INFO("silentaim", "sendMovementPackets hooked and verified -- silent rotation is live");

    // Seed the wire tracker from the player's rotation: the server holds
    // whatever the last look carried, and with nothing spoofed yet that is
    // the real view. No prev until the first observed look.
    if (g_cache.entity_yaw_fid && g_cache.entity_pitch_fid) {
        const jfloat y = env->GetFloatField(local_player, g_cache.entity_yaw_fid);
        const jfloat p = env->GetFloatField(local_player, g_cache.entity_pitch_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else {
            g_wire_yaw = y; g_wire_pitch = p; g_wire_have = true;
            g_wire_prev_yaw = y; g_wire_prev_pitch = p; g_wire_have_prev = false;
        }
    }
    VLOG_INFO("silentaim", "wire tracker: lastYaw={} lastPitch={} ticksSincePos={} lastXYZ={} seeded yaw {} pitch {}",
              g_cache.player_last_yaw_fid ? 1 : 0, g_cache.player_last_pitch_fid ? 1 : 0,
              g_cache.player_ticks_since_pos_fid ? 1 : 0,
              (g_cache.player_last_x_fid && g_cache.player_last_base_y_fid && g_cache.player_last_z_fid) ? 1 : 0,
              g_wire_yaw, g_wire_pitch);

    // No second hook. The pre-tick anchor rides the triggerbot's existing
    // handleBlockBreaking detour instead -- see on_pre_player_tick().
    //
    // Two attempts at owning that phase failed, and both failures are worth
    // keeping written down:
    //   ClientPlayerEntity.tickMovement -- public override. Attaches, then the
    //     original silently no-ops (CallNonvirtual* re-enters the detour and
    //     JNIHook's recursion guard eats it). The player would freeze.
    //   MinecraftClient.handleInputEvents -- private, but a DISPATCHER. The
    //     original throws partway through, because the class copy cannot make
    //     the this.doAttack() / this.setScreen() calls it is built out of.
    //     Movement correction worked and attacks stopped.
    // What is left is: private AND leaf. handleBlockBreaking is both, and the
    // triggerbot already has it hooked and proven.
    if (!g_cache.mc_options_fid || !g_cache.key_binding_pressed_fid)
        VLOG_WARN("silentaim", "movement correction unavailable (options/KeyBinding "
                               "not resolved) -- Simulation may flag while moving");
}

// â”€â”€ cache â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void init_cache(JNIEnv* env)
{
    if (!env || g_cache.initialized) return;
    std::scoped_lock lk(s_cache_mx);
    if (g_cache.initialized) return;

    if (!cache_class(env, sdk::mappings::minecraftclass_sig, g_cache.minecraft_class))
        return;

    g_cache.mc_player_fid = cache_field(env, g_cache.minecraft_class,
        sdk::mappings::player_name, sdk::mappings::player_sig);
    g_cache.mc_world_fid = cache_field(env, g_cache.minecraft_class,
        sdk::mappings::world_name, sdk::mappings::world_sig);
    g_cache.mc_current_screen_fid = cache_field(env, g_cache.minecraft_class,
        "field_1755", "Lnet/minecraft/class_437;");

    cache_class(env, sdk::mappings::clientplayerentity_class_sig, g_cache.client_player_class);
    if (g_cache.client_player_class) {
        g_cache.player_render_yaw_fid = cache_field(env, g_cache.client_player_class,
            sdk::mappings::render_yaw_name, sdk::mappings::render_yaw_sig);
        g_cache.player_render_pitch_fid = cache_field(env, g_cache.client_player_class,
            sdk::mappings::render_pitch_name, sdk::mappings::render_pitch_sig);
        VLOG_INFO("silentaim", "held-item sway fields: renderYaw={} renderPitch={}",
                  g_cache.player_render_yaw_fid ? 1 : 0,
                  g_cache.player_render_pitch_fid ? 1 : 0);

        // The wire tracker's fields (see the header). Intermediary names are
        // the same on every 1.21.x (checked 1.21 .. 1.21.11); the Mojang
        // names are the fallback for a host without intermediaries.
        g_cache.player_last_yaw_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3941", "yRotLast", "F");
        g_cache.player_last_pitch_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3925", "xRotLast", "F");
        g_cache.player_ticks_since_pos_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3923", "positionReminder", "I");
        g_cache.player_last_x_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3926", "xLast", "D");
        g_cache.player_last_base_y_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3940", "yLast1", "D");
        g_cache.player_last_z_fid = cache_field_either(env, g_cache.client_player_class,
            "field_3924", "zLast", "D");
    }
    cache_class(env, sdk::mappings::entity_class_sig,             g_cache.entity_class);
    cache_class(env, sdk::mappings::player_entity_class_sig,      g_cache.player_entity_class);
    cache_class(env, "net/minecraft/class_1309",                  g_cache.living_entity_class);
    cache_class(env, "net/minecraft/class_638",                   g_cache.world_class);

    if (g_cache.entity_class) {
        g_cache.entity_get_x_mid = cache_method(env, g_cache.entity_class,
            sdk::mappings::entity_get_x_name, sdk::mappings::entity_get_x_sig);
        g_cache.entity_get_y_mid = cache_method(env, g_cache.entity_class,
            sdk::mappings::entity_get_y_name, sdk::mappings::entity_get_y_sig);
        g_cache.entity_get_z_mid = cache_method(env, g_cache.entity_class,
            sdk::mappings::entity_get_z_name, sdk::mappings::entity_get_z_sig);

        // Entity.yaw / Entity.pitch. NOT the render-interpolated pair -- the
        // packet reads these, and so does everything else that matters.
        g_cache.entity_yaw_fid   = cache_field_either(env, g_cache.entity_class,
            "field_6031", "yRot", "F");
        g_cache.entity_pitch_fid = cache_field_either(env, g_cache.entity_class,
            "field_5965", "xRot", "F");
        g_cache.entity_prev_yaw_fid   = cache_field_either(env, g_cache.entity_class,
            "field_5982", "yRotO", "F");
        g_cache.entity_prev_pitch_fid = cache_field_either(env, g_cache.entity_class,
            "field_6004", "xRotO", "F");

        g_cache.entity_is_alive_mid = cache_method_either(env, g_cache.entity_class,
            "method_5805", "isAlive", "()Z");
        g_cache.entity_get_eye_height_mid = cache_method_either(env, g_cache.entity_class,
            "method_5751", "getEyeHeight", "()F");
        g_cache.entity_is_invisible_mid = cache_method_either(env, g_cache.entity_class,
            "method_5767", "isInvisible", "()Z");
        g_cache.entity_is_sprinting_mid = cache_method_either(env, g_cache.entity_class,
            "method_5624", "isSprinting", "()Z");
        g_cache.entity_is_sneaking_mid = cache_method_either(env, g_cache.entity_class,
            "method_5715", "isShiftKeyDown", "()Z");
        VLOG_INFO("silentaim", "Entity.isInvisible: {}",
                  g_cache.entity_is_invisible_mid ? "resolved" : "NOT FOUND");
    }

    if (g_cache.living_entity_class) {
        g_cache.living_get_health_mid = cache_method_either(env, g_cache.living_entity_class,
            "method_6032", "getHealth", "()F");
        g_cache.living_is_blocking_mid = cache_method_either(env, g_cache.living_entity_class,
            "method_6039", "isBlocking", "()Z");

        // Render-only rotation. A wrong NAME here cannot corrupt anything --
        // GetFieldID either finds the field or returns null, and names are
        // unique per class, so there is no way to land on a different float
        // by accident. If they do not resolve, the F5 view simply does not
        // follow and everything else is unaffected.
        g_cache.living_body_yaw_fid = cache_field_either(env, g_cache.living_entity_class,
            "field_6283", "yBodyRot", "F");
        g_cache.living_head_yaw_fid = cache_field_either(env, g_cache.living_entity_class,
            "field_6241", "yHeadRot", "F");

        // The prev* pair is NOT resolved on purpose. Their intermediary names
        // were guesswork, LivingEntity's float fields around them drive the
        // hand-swing and limb animation, and vanilla maintains prev* correctly
        // on its own. Leaving them null makes it impossible to write one by
        // accident later.
        g_cache.living_prev_body_yaw_fid = nullptr;
        g_cache.living_prev_head_yaw_fid = nullptr;

        VLOG_INFO("silentaim", "third-person rotation fields: body={} head={}",
                  g_cache.living_body_yaw_fid ? 1 : 0,
                  g_cache.living_head_yaw_fid ? 1 : 0);
    }

    // Vanilla's attack / use footprints for the same-tick packet-order gate
    // (see vanilla_acted_this_tick). class_310.field_1752 itemUseCooldown I
    // and class_1657.field_6273 lastAttackedTicks I on both 1.21 and 1.21.11.
    g_cache.mc_item_use_cooldown_fid = cache_field_either(env, g_cache.minecraft_class,
        "field_1752", "rightClickDelay", "I");
    if (g_cache.player_entity_class)
        g_cache.player_last_attacked_ticks_fid = cache_field_either(env, g_cache.player_entity_class,
            "field_6273", "attackStrengthTicker", "I");

    // Movement correction plumbing.
    g_cache.mc_options_fid = cache_field_either(env, g_cache.minecraft_class,
        "field_1690", "options", "Lnet/minecraft/class_315;");
    if (cache_class(env, "net/minecraft/class_304", g_cache.key_binding_class)) {
        g_cache.key_binding_translation_mid = cache_method_either(env,
            g_cache.key_binding_class, "method_1431", "getName", "()Ljava/lang/String;");
        g_cache.key_binding_pressed_fid =
            find_sole_boolean_field(env, g_cache.key_binding_class);
    }
    VLOG_INFO("silentaim", "movement correction plumbing: options={} keybind={} pressed={}",
              g_cache.mc_options_fid ? 1 : 0,
              g_cache.key_binding_translation_mid ? 1 : 0,
              g_cache.key_binding_pressed_fid ? 1 : 0);

    // Mouse lattice from the option (see refresh_mouse_lattice). Names verified
    // against the 1.21 and 1.21.11 tiny caches: class_315.method_42495
    // ()Lclass_7172; getMouseSensitivity, class_7172.method_41753
    // ()Ljava/lang/Object; getValue, class_315.field_1914 Z smoothCameraEnabled.
    {
        jclass options_cls = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_315");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (options_cls) {
            g_cache.options_get_sensitivity_mid = cache_method_either(env, options_cls,
                "method_42495", "sensitivity", "()Lnet/minecraft/class_7172;");
            g_cache.options_mouse_sensitivity_fid = cache_field_either(env, options_cls,
                "field_1843", "mouseSensitivity", "Lnet/minecraft/class_7172;");
            g_cache.options_smooth_camera_fid = cache_field_either(env, options_cls,
                "field_1914", "smoothCamera", "Z");
            env->DeleteLocalRef(options_cls);
        }
        jclass option_cls = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_7172");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (option_cls) {
            g_cache.option_get_value_mid = cache_method_either(env, option_cls,
                "method_41753", "get", "()Ljava/lang/Object;");
            env->DeleteLocalRef(option_cls);
        }
        jclass dbl = env->FindClass("java/lang/Double");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (dbl) {
            g_cache.double_value_mid = env->GetMethodID(dbl, "doubleValue", "()D");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(dbl);
        }
    }
    VLOG_INFO("silentaim", "cache: options={} sens={} getValue={} smooth={} doubleValue={} sprinting={} sneaking={} useCooldown={} lastAttacked={} -> ready",
              g_cache.mc_options_fid ? 1 : 0,
              g_cache.options_get_sensitivity_mid ? 1 : 0,
              g_cache.option_get_value_mid ? 1 : 0,
              g_cache.options_smooth_camera_fid ? 1 : 0,
              g_cache.double_value_mid ? 1 : 0,
              g_cache.entity_is_sprinting_mid ? 1 : 0,
              g_cache.entity_is_sneaking_mid ? 1 : 0,
              g_cache.mc_item_use_cooldown_fid ? 1 : 0,
              g_cache.player_last_attacked_ticks_fid ? 1 : 0);

    // Held-item chain for Only Weapon. Resolved best-effort: if any link is
    // missing the gate opens rather than closes, so a mapping gap can never
    // silently disable the module.
    if (g_cache.player_entity_class) {
        g_cache.player_inventory_fid = cache_field_either(env, g_cache.player_entity_class,
            "field_7514", "inventory", "Lnet/minecraft/class_1661;");
    }
    {
        jclass inv = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1661");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (inv) {
            g_cache.inventory_selected_slot_fid =
                cache_field_either(env, inv, "field_7545", "selected", "I");
            g_cache.inventory_get_stack_mid =
                cache_method_either(env, inv, "method_5438", "getItem",
                                    "(I)Lnet/minecraft/class_1799;");
            env->DeleteLocalRef(inv);
        }
        jclass stack = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1799");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (stack) {
            g_cache.itemstack_is_empty_mid =
                cache_method_either(env, stack, "method_7960", "isEmpty", "()Z");
            g_cache.itemstack_get_item_mid =
                cache_method_either(env, stack, "method_7909", "getItem",
                                    "()Lnet/minecraft/class_1792;");
            env->DeleteLocalRef(stack);
        }
        jclass item = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1792");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (item) {
            g_cache.item_get_translation_key_mid =
                cache_method_either(env, item, "method_7876", "getDescriptionId",
                                    "()Ljava/lang/String;");
            env->DeleteLocalRef(item);
        }
    }

    if (g_cache.world_class) {
        g_cache.world_get_entities_mid = cache_method_either(env, g_cache.world_class,
            "method_18112", "entitiesForRendering", "()Ljava/lang/Iterable;");
    }

    {
        jclass iterable = env->FindClass("java/lang/Iterable");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (iterable) {
            g_cache.iterable_iterator_mid =
                env->GetMethodID(iterable, "iterator", "()Ljava/util/Iterator;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(iterable);
        }
        jclass iterator = env->FindClass("java/util/Iterator");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (iterator) {
            g_cache.iterator_has_next_mid = env->GetMethodID(iterator, "hasNext", "()Z");
            g_cache.iterator_next_mid     = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(iterator);
        }
    }

    bool valid = g_cache.minecraft_class && g_cache.mc_player_fid;
    g_cache.initialized = valid;
    VLOG_INFO("silentaim",
              "cache built: yaw={} pitch={} entities={} living={} player_cls={}",
              g_cache.entity_yaw_fid ? 1 : 0,
              g_cache.entity_pitch_fid ? 1 : 0,
              g_cache.world_get_entities_mid ? 1 : 0,
              g_cache.living_entity_class ? 1 : 0,
              g_cache.client_player_class ? 1 : 0);
}

// Separate function: shutdown_cache uses __try, and the std::function
// temporaries here need unwinding, which MSVC refuses to mix with SEH.
static void clear_action_registrations()
{
    g_action_owner = nullptr;
    g_action_easing = false;
    g_action_release_after_send = false;
    g_action_hold_until = 0;
    g_vanilla_acted_tick = ~0u;
    reset_sent_history(0.f, 0.f, false);
    for (auto& r : g_action_regs) { r.owner = nullptr; r.cb = ActionCallbacks{}; }
}

void shutdown_cache()
{
    clear_action_registrations();
    g_shutting_down.store(true, std::memory_order_release);

    JNIEnv* env = vanta::jvm::thisThreadEnv();
    if (!env) return;

    if (g_hie_attached && g_hie_method_id) {
        __try { JNIHook_Detach(g_hie_method_id); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_hook_attached && g_smp_method_id) {
        __try { JNIHook_Detach(g_smp_method_id); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_hie_attached           = false;
    ORIG_handleInputEvents   = nullptr;
    g_hie_method_id          = nullptr;
    g_hook_attached          = false;
    g_probe_passed           = false;
    ORIG_sendMovementPackets = nullptr;
    g_smp_method_id          = nullptr;

    auto drop = [&](jclass& c) { if (c) { env->DeleteGlobalRef(c); c = nullptr; } };
    drop(g_cache.minecraft_class);
    drop(g_cache.client_player_class);
    drop(g_cache.entity_class);
    drop(g_cache.player_entity_class);
    drop(g_cache.living_entity_class);
    drop(g_cache.world_class);
    drop(g_cache.key_binding_class);

    g_cache.initialized = false;
}

void ensure_installed()
{
    if (g_shutting_down.load(std::memory_order_acquire)) return;

    JNIEnv* env = vanta::jvm::thisThreadEnv();
    if (!env) return;

    if (!g_cache.initialized) { init_cache(env); return; }

    // Safety net: the movement frame is opened at the front of the tick and is
    // supposed to be closed by sendMovementPackets at the back of it. If that
    // method did not run -- riding a vehicle is the realistic case -- the frame
    // would stay open forever, pinning the camera to the aim and feeding the
    // spoofed angle to the lattice estimator. This runs after the tick, so a
    // frame still open here has leaked; close it against the live player.
    if (g_frame_open && g_cache.mc_player_fid) {
        jfieldID inst = env->GetStaticFieldID(g_cache.minecraft_class,
            sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jobject mc0 = inst ? env->GetStaticObjectField(g_cache.minecraft_class, inst) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); mc0 = nullptr; }
        if (mc0) {
            jobject p0 = env->GetObjectField(mc0, g_cache.mc_player_fid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); p0 = nullptr; }
            if (p0) { close_movement_frame(env, p0); env->DeleteLocalRef(p0); }
            env->DeleteLocalRef(mc0);
        }
        g_frame_open = false;   // never let a failed close wedge it
    }

    // The tickMovement probe runs inside the hook, on a real call, and can only
    // report failure after the fact. Tear it down here -- off the hook's own
    // stack -- the moment it does. One frozen tick, not a frozen session.
    if (g_hie_attached && g_hie_broken.load(std::memory_order_acquire)) {
        __try { JNIHook_Detach(g_hie_method_id); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_hie_attached         = false;
        ORIG_handleInputEvents = nullptr;
        g_hie_method_id        = nullptr;
        VLOG_WARN("silentaim", "handleInputEvents detached -- movement correction is off, "
                               "Simulation may flag while moving");
    }

    if (!g_cache.mc_player_fid) return;

    jfieldID inst = env->GetStaticFieldID(g_cache.minecraft_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!inst) return;

    jobject mc = env->GetStaticObjectField(g_cache.minecraft_class, inst);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;

    // Needs a live GameOptions, so it cannot run from init_cache.
    resolve_movement_keys(env, mc);

    if (!g_hook_attached) {
        jobject player = env->GetObjectField(mc, g_cache.mc_player_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }
        if (player) {
            install_rotation_hook(env, player);
            env->DeleteLocalRef(player);
        }
    }
    env->DeleteLocalRef(mc);
}

} // namespace vanta::modules::silentaim
