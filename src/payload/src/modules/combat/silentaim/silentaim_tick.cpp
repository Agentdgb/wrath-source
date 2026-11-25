// silentaim_tick.cpp â€” target selection, the aim controller, and the pump.
//
// This is the port of SilentAura.updateTarget() / updateAim(). The controller
// is Vape's, gain for gain: a PI loop on the yaw/pitch error with the previous
// step fed back as a damping term, per-session randomized gain scales, and a
// slow random-walk jitter on the aim point. The numbers are deliberately not
// "cleaned up" â€” they are what makes the motion read as a hand.

#include "silentaim.h"
#include "silentaim_tick.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "modules/combat/triggerbot/triggerbot_hook.h"
#include "core/social/EntityFriends.h"   // a friend is not a target
#include "vgui/bridge.h"               // Freecam gate

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <utility>
#include <windows.h>

namespace dt::modules::silentaim_tick {

namespace tb = ::dt::modules::silentaim;

static constexpr double kPi     = 3.14159265358979323846;
static constexpr double kDeg    = 180.0 / kPi;
static constexpr float  TICK_MS = 50.f;

// Vanilla eye height and hitbox for an adult player. The aura's dominant case
// is PvP, and the triggerbot already assumes the same box for its raycast.
static constexpr double EYE_HEIGHT   = 1.62;
static constexpr double PLAYER_HALF_W = 0.30;
static constexpr double PLAYER_HEIGHT = 1.80;

// Vape's SilentAura defaults, pinned rather than exposed. These shape how the
// motion reads, and a wrong value here does not fail loudly â€” it just makes
// the aim look machine-made, which is the one thing that matters.
static constexpr double AIM_SPEED = 7.0;   // "Aim speed" 1..10

// How far inside the engagement range a hit is actually permitted. Covers the
// drift between this pump computing the lock and the server evaluating the
// attack a couple of ticks later.
static constexpr double LOCK_REACH_MARGIN = 0.18;

// No target is ever aimed at beyond this angle from the player's view, whatever
// the FOV setting says. Keeps the yaw fold clear of the +/-180 seam, which is
// an invariant rather than a preference. See update_aim.
static constexpr double MAX_RETENTION_ANGLE = 140.0;

// â”€â”€ RAII local ref â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
namespace {
class LocalRef {
public:
    LocalRef(JNIEnv* env, jobject ref) noexcept : env_(env), ref_(ref) {}
    ~LocalRef() { if (env_ && ref_) env_->DeleteLocalRef(ref_); }
    LocalRef(const LocalRef&)            = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    LocalRef(LocalRef&& o) noexcept : env_(o.env_), ref_(o.ref_) { o.ref_ = nullptr; }

    jobject get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }
private:
    JNIEnv* env_;
    jobject ref_;
};
} // namespace

static std::mt19937& rng()
{
    static std::mt19937 g(std::random_device{}());
    return g;
}

static double urand(double lo, double hi)
{
    std::uniform_real_distribution<double> d(lo, hi);
    return d(rng());
}

// â”€â”€ aim jitter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Port of SilentAuraAimJitter. A target offset is re-rolled every 100-1000 ms
// and the current offset walks toward it 0.01-0.06 per tick. The point is that
// the aim point DRIFTS rather than dithering: per-tick white noise is trivially
// separable from a hand, a slow walk is not.
namespace {
class AimJitter {
public:
    AimJitter(double lo, double hi) : lo_(lo), hi_(hi) {}

    void update()
    {
        const unsigned long long now = GetTickCount64();
        if (now >= next_retarget_ms_) {
            next_retarget_ms_ = now + static_cast<unsigned long long>(urand(100.0, 1000.0));
            target_ = urand(lo_, hi_);
        }
        // Approach as a decaying FRACTION of what is left, not by a near-fixed
        // step. Vape's original walks toward the target at 0.01-0.06 per tick,
        // which is a straight ramp -- and once the aim has settled on a
        // stationary target that ramp IS the rotation, so the sent deltas are a
        // line. That is what Vulcan's Aim (Linear) measures. A proportional
        // approach spends the same distance with no two ticks alike.
        const double frac = 0.06 + urand(0.0, 0.12);
        current_ += (target_ - current_) * frac;
    }

    double value() const { return current_; }
    void   reset() { current_ = 0.0; target_ = 0.0; next_retarget_ms_ = 0; }

private:
    double lo_, hi_;
    double current_ = 0.0;
    double target_  = 0.0;
    unsigned long long next_retarget_ms_ = 0;
};
} // namespace

static AimJitter g_jitter_x   { -0.15, 0.15 };
static AimJitter g_jitter_z   { -0.15, 0.15 };
static AimJitter g_jitter_pitch{ -0.30, 0.25 };

// â”€â”€ controller state â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static bool   g_has_target      = false;
static bool   g_on_target       = false;

static float  g_managed_yaw     = 0.f;
static float  g_managed_pitch   = 0.f;
static float  g_prev_managed_yaw   = 0.f;
static float  g_prev_managed_pitch = 0.f;

static float  g_yaw_integral    = 0.f;
static float  g_pitch_integral  = 0.f;

// True while the player's own crosshair already lands on the target, in which
// case nothing is spoofed at all -- see apply_rotation.
static bool g_player_already_on = false;

// The rotation that actually went out last tick, after the divergence cap and
// the lattice snap. This -- not the controller's internal target -- is what the
// server has, so it is what the lock test must be judged against.
static float g_sent_yaw   = 0.f;
static float g_sent_pitch = 0.f;
static bool  g_have_sent  = false;

// Re-rolled every time the target changes, so two engagements never share a
// gain profile. Vape's randomizeGainFactors().
static float  g_pitch_p_scale   = 1.f;
static float  g_pitch_i_scale   = 1.f;
static float  g_yaw_p_scale     = 1.f;
static float  g_yaw_i_scale     = 1.f;

// Previous-frame bookkeeping used instead of reading Entity.velocity, so the
// module needs no Vec3d mappings at all.
static bool   g_have_prev_player_pos = false;
static double g_prev_px = 0.0, g_prev_py = 0.0, g_prev_pz = 0.0;
static bool   g_have_prev_target_pos = false;
static double g_prev_tx = 0.0, g_prev_tz = 0.0;

// Identity of the current target, so a switch can be detected. Global ref;
// owned here and released on replace.
static jobject g_target_ref = nullptr;


// Ticks spent easing the rotation back to the player's own view after the
// target is gone. Short on purpose -- long enough to remove the step, not long
// enough to keep aiming at something that is no longer there.
static int g_release_ticks = 0;
// Enough ticks for the widest offset the retention cap allows to decay to
// nothing: 140 degrees at 0.55 per tick reaches under a quarter of a degree in
// about nine. Cutting it short would leave the very step this exists to remove.
static constexpr int RELEASE_TICKS = 10;

// Consecutive ticks the SENT rotation has actually been pointing at the target.
// The target is not handed to the attack path until this clears, so a click
// during a swing can never be redirected to an entity the server does not yet
// believe we are looking at.
static int g_locked_ticks = 0;

// Two, not one, and the reason is packet order inside MinecraftClient.tick():
// handleInputEvents() -> doAttack() runs BEFORE world.tickEntities() ->
// sendMovementPackets(). The attack therefore leaves ahead of that tick's move
// packet, and the server validates it against the PREVIOUS tick's rotation.
// Requiring two consecutive on-target ticks guarantees a matching rotation has
// already reached the server before any attack is redirected.
static constexpr int REQUIRED_LOCK_TICKS = 2;

static void randomize_gain_factors()
{
    g_pitch_p_scale = static_cast<float>(0.85 + urand(0.0, 1.0) * 0.30);
    g_pitch_i_scale = static_cast<float>(0.85 + urand(0.0, 1.0) * 0.30);
    g_yaw_p_scale   = static_cast<float>(0.80 + urand(0.0, 1.0) * 0.40);
    g_yaw_i_scale   = static_cast<float>(0.85 + urand(0.0, 1.0) * 0.30);
}

static float wrap180(float d)
{
    d = std::fmod(d + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    return d - 180.f;
}

static void clear_target(JNIEnv* env)
{
    if (g_target_ref) {
        if (env) {
            env->DeleteGlobalRef(g_target_ref);
        } else {
            dt::jvm::JniScope scope;
            if (scope) scope.get()->DeleteGlobalRef(g_target_ref);
        }
    }
    g_target_ref = nullptr;
}


void reset()
{
    JNIEnv* env = dt::jvm::thisThreadEnv();
    clear_target(env);

    g_has_target     = false;
    g_on_target      = false;
    g_release_ticks  = 0;
    g_locked_ticks   = 0;
    g_yaw_integral   = 0.f;
    g_pitch_integral = 0.f;
    g_have_prev_target_pos = false;
    g_player_already_on = false;
    g_have_sent         = false;
    g_jitter_x.reset();
    g_jitter_z.reset();
    g_jitter_pitch.reset();

    tb::set_managed_rotation(false, 0.f, 0.f, 0.f);
    if (env) ::dt::modules::triggerbot_hook::set_silentaim_target(env, nullptr);
}

// â”€â”€ entity reads â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static bool entity_pos(JNIEnv* env, const tb::jni_cache& c, jobject e,
                       double& x, double& y, double& z)
{
    if (!c.entity_get_x_mid || !c.entity_get_y_mid || !c.entity_get_z_mid) return false;
    x = env->CallDoubleMethod(e, c.entity_get_x_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    y = env->CallDoubleMethod(e, c.entity_get_y_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    z = env->CallDoubleMethod(e, c.entity_get_z_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static bool entity_alive(JNIEnv* env, const tb::jni_cache& c, jobject e)
{
    if (c.entity_is_alive_mid) {
        jboolean a = env->CallBooleanMethod(e, c.entity_is_alive_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (a != JNI_TRUE) return false;
    }
    if (c.living_entity_class && c.living_get_health_mid &&
        env->IsInstanceOf(e, c.living_entity_class) == JNI_TRUE) {
        jfloat hp = env->CallFloatMethod(e, c.living_get_health_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (hp <= 0.f) return false;
    }
    return true;
}

static float entity_health(JNIEnv* env, const tb::jni_cache& c, jobject e)
{
    if (!c.living_entity_class || !c.living_get_health_mid) return 20.f;
    if (env->IsInstanceOf(e, c.living_entity_class) != JNI_TRUE) return 20.f;
    jfloat hp = env->CallFloatMethod(e, c.living_get_health_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 20.f; }
    return static_cast<float>(hp);
}

static bool entity_blocking(JNIEnv* env, const tb::jni_cache& c, jobject e)
{
    if (!c.living_entity_class || !c.living_is_blocking_mid) return false;
    if (env->IsInstanceOf(e, c.living_entity_class) != JNI_TRUE) return false;
    jboolean b = env->CallBooleanMethod(e, c.living_is_blocking_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return b == JNI_TRUE;
}

// Refuse invisible targets.
//
// This is not a preference, it is a detection countermeasure. Gladiator's
// HoneypotCheck spawns a client-side-only fake player one block BEHIND you
// shortly after your first attack, with metadata byte 0 = 32 (the invisibility
// flag), lets it live about 300 ms, and flags KillAura the instant anything
// attacks it. Its "grace period" does not protect you either -- the expression
// reduces to a single tick:
//
//     abs(currentTick - joinTick) * 20 * 60 <= 60    ->    delta <= 0.05
//
// A 180 degree cone excludes something directly behind, but only until the
// player turns, which in a fight is constantly; the moment it comes round the
// front it is the nearest entity in the world at one block and wins target
// selection outright. Nothing in the entity itself says "I am bait" -- except
// that it is invisible.
//
// Which is also why this is the honest filter rather than a hack: a human
// cannot aim at what they cannot see, so an aura that happily tracks invisible
// entities is doing something no legitimate input could. Refusing them costs a
// capability that was never defensible and removes the bait at the same time.
//
// Unresolvable mapping -> return false, i.e. do not filter. A silent aim that
// suddenly refuses every target because one method id came back null is worse
// than one that can be baited.
static bool entity_is_invisible(JNIEnv* env, const tb::jni_cache& c, jobject e)
{
    if (!c.entity_is_invisible_mid) return false;
    jboolean v = env->CallBooleanMethod(e, c.entity_is_invisible_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return v == JNI_TRUE;
}

// â”€â”€ aim point â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// SilentAura.computeAimCoords. Center returns the entity origin; Closest walks
// the hitbox for the nearest point to the player, which is what lets the aura
// hold a target at the edge of reach without over-rotating.
// Pinned to Vape's default, Center -- but "centre" horizontally and UPPER BODY
// vertically, never the entity origin.
//
// Entity Y is the FEET. Returning it made every range and angle test measure to
// the ground at the target's position, which on level terrain adds a constant
// 1.62 of vertical distance and eats that much of the 3.0 budget: the effective
// horizontal reach collapsed to about 2.5 blocks. Targets vanilla could hit
// were dropped, the module unpublished them, and the click fell through to the
// real crosshair -- which is the "sometimes it just misses" everyone sees.
//
// This matches the Y update_aim() already aims at, so selection and aiming
// finally agree on the same point.
static void compute_aim_point(const tb::jni_cache&, double, double py, double,
                              double tx, double ty, double tz,
                              double& ax, double& ay, double& az)
{
    const double eye_y = py + EYE_HEIGHT;
    ax = tx;
    az = tz;
    ay = (eye_y < ty) ? ty
                      : std::min(eye_y, ty + PLAYER_HEIGHT) - 0.275;
}

// Does a ray along the ROTATION WE ARE SENDING actually enter the target's box?
//
// This is the same question the server asks when it validates an attack, so
// asking it the same way is what makes the lock mean anything. The previous
// test was an angular tolerance plus `distance-to-aim-point <= reach`, and that
// second half was simply the wrong quantity: target selection measures to the
// HITBOX, so an enemy at 3.2 blocks was selected happily (box distance ~2.9)
// and then never satisfied the lock (aim-point distance 3.2 > 3.0). It could
// never be published, so every swing at it missed -- and Extra Reach widened
// the dead band instead of closing it.
static bool rotation_hits_target(double px, double py, double pz,
                                 float yaw, float pitch,
                                 double tx, double ty, double tz,
                                 double reach)
{
    const double ey = py + EYE_HEIGHT;
    const double yr = yaw * (kPi / 180.0), pr = pitch * (kPi / 180.0);
    const double cp = std::cos(pr);
    const double dx = -std::sin(yr) * cp;
    const double dy = -std::sin(pr);
    const double dz =  std::cos(yr) * cp;

    // Shrink the hitbox slightly (0.03 blocks) rather than expanding it.
    // Minecraft 1.9+ / 1.21.11 has 0.0 targeting margin on players, and
    // Spartan's HIT_REACH raytraces against the strict 0.6 x 1.8 server box:
    // the old +0.10 pad marked rotations outside the real hitbox as "on target",
    // firing attacks that missed the server HitBox ("Missing HitBox of attackable").
    constexpr double kInset = 0.03;
    const double mnx = tx - (PLAYER_HALF_W - kInset), mxx = tx + (PLAYER_HALF_W - kInset);
    const double mny = ty + kInset,                   mxy = ty + PLAYER_HEIGHT - kInset;
    const double mnz = tz - (PLAYER_HALF_W - kInset), mxz = tz + (PLAYER_HALF_W - kInset);

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
    if (!slab(px, dx, mnx, mxx)) return false;
    if (!slab(ey, dy, mny, mxy)) return false;
    if (!slab(pz, dz, mnz, mxz)) return false;
    if (t_far < 0.0) return false;

    const double t = t_near < 0.0 ? 0.0 : t_near;
    return t <= reach;
}

// Eye to the nearest point on the target's box -- the same quantity vanilla
// uses for attack reach, so a target we accept is a target the server accepts.
static double eye_to_hitbox_distance(double px, double py, double pz,
                                     double tx, double ty, double tz)
{
    const double eye_y = py + EYE_HEIGHT;
    const double cx = std::clamp(px,    tx - PLAYER_HALF_W, tx + PLAYER_HALF_W);
    const double cy = std::clamp(eye_y, ty,                 ty + PLAYER_HEIGHT);
    const double cz = std::clamp(pz,    tz - PLAYER_HALF_W, tz + PLAYER_HALF_W);
    const double dx = px - cx, dy = eye_y - cy, dz = pz - cz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Angle between the player's CURRENT view and the direction to a point.
// SilentAura uses this both as the max-angle gate and as the Angle sort key.
static double angle_to_point(float yaw, float pitch,
                             double px, double py, double pz,
                             double tx, double ty, double tz)
{
    const double ey = py + EYE_HEIGHT;
    const double dx = tx - px, dy = ty - ey, dz = tz - pz;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6) return 0.0;

    const double yr = yaw * (kPi / 180.0), pr = pitch * (kPi / 180.0);
    const double cp = std::cos(pr);
    const double fx = -std::sin(yr) * cp;
    const double fy = -std::sin(pr);
    const double fz =  std::cos(yr) * cp;

    double dot = (dx * fx + dy * fy + dz * fz) / len;
    dot = std::clamp(dot, -1.0, 1.0);
    return std::acos(dot) * kDeg;
}

// Only Weapon. Matches by translation key rather than by item identity, the
// same way the triggerbot does â€” one string test covers every material tier
// and every modded weapon that names itself conventionally.
//
// Opens on any failure. A gate that closes when a mapping is missing would
// turn the whole module off silently, which is much worse than aiming while
// holding a pickaxe.
static bool holding_weapon(JNIEnv* env, const tb::jni_cache& c, jobject player)
{
    if (!tb::settings::only_weapon.load(std::memory_order_relaxed)) return true;

    if (!c.player_inventory_fid || !c.inventory_selected_slot_fid ||
        !c.inventory_get_stack_mid || !c.itemstack_is_empty_mid ||
        !c.itemstack_get_item_mid || !c.item_get_translation_key_mid) return true;

    LocalRef inv(env, env->GetObjectField(player, c.player_inventory_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!inv) return true;

    const jint slot = env->GetIntField(inv.get(), c.inventory_selected_slot_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }

    LocalRef stack(env, env->CallObjectMethod(inv.get(), c.inventory_get_stack_mid, slot));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!stack) return true;

    if (env->CallBooleanMethod(stack.get(), c.itemstack_is_empty_mid) == JNI_TRUE) return false;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }

    LocalRef item(env, env->CallObjectMethod(stack.get(), c.itemstack_get_item_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!item) return true;

    LocalRef key(env, env->CallObjectMethod(item.get(), c.item_get_translation_key_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!key) return true;

    const char* k = env->GetStringUTFChars(static_cast<jstring>(key.get()), nullptr);
    if (!k) return true;
    const bool is_weapon = std::strstr(k, "sword")   || std::strstr(k, "axe")
                        || std::strstr(k, "mace")    || std::strstr(k, "trident");
    env->ReleaseStringUTFChars(static_cast<jstring>(key.get()), k);
    return is_weapon;
}

static double attack_range()
{
    return 3.0 + tb::settings::extra_reach_x10.load(std::memory_order_relaxed) / 10.0;
}

// â”€â”€ target selection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct Candidate {
    jobject ent;
    double  distance;
    double  angle;
    float   health;
};

static jobject pick_target(JNIEnv* env, const tb::jni_cache& c, jobject mc,
                           jobject player, double px, double py, double pz,
                           float yaw, float pitch)
{
    if (!c.mc_world_fid || !c.world_get_entities_mid ||
        !c.iterable_iterator_mid || !c.iterator_has_next_mid || !c.iterator_next_mid)
        return nullptr;

    LocalRef world(env, env->GetObjectField(mc, c.mc_world_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!world) return nullptr;

    LocalRef iterable(env, env->CallObjectMethod(world.get(), c.world_get_entities_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!iterable) return nullptr;

    LocalRef it(env, env->CallObjectMethod(iterable.get(), c.iterable_iterator_mid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!it) return nullptr;

    const double range      = attack_range();
    const double half_angle = tb::settings::max_angle.load(std::memory_order_relaxed) / 2.0;
    const bool   shield_check = tb::settings::shield_check.load(std::memory_order_relaxed);
    const int    targets = tb::settings::targets.load(std::memory_order_relaxed);

    Candidate best{ nullptr, 1e30, 1e30, 1e30f };
    bool have_best = false;

    for (;;) {
        jboolean more = env->CallBooleanMethod(it.get(), c.iterator_has_next_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (more != JNI_TRUE) break;

        LocalRef ent(env, env->CallObjectMethod(it.get(), c.iterator_next_mid));
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!ent) continue;

        if (env->IsSameObject(ent.get(), player) == JNI_TRUE) continue;

        if (c.living_entity_class &&
            env->IsInstanceOf(ent.get(), c.living_entity_class) != JNI_TRUE) continue;

        if (c.player_entity_class) {
            const bool is_player =
                env->IsInstanceOf(ent.get(), c.player_entity_class) == JNI_TRUE;
            switch (static_cast<tb::Targets>(targets)) {
            case tb::Targets::Players: if (!is_player) continue; break;
            case tb::Targets::Mobs:    if ( is_player) continue; break;
            case tb::Targets::All:
            default:                                            break;
            }
        }

        // A friend is not a target. One entity-id probe on the hot path, and the
        // whole call short-circuits to false while nobody is marked.
        if (::dt::social::isFriend(env, ent.get())) continue;

        if (!entity_alive(env, c, ent.get())) continue;
        if (entity_is_invisible(env, c, ent.get())) continue;   // honeypot bait
        if (shield_check && entity_blocking(env, c, ent.get())) continue;

        double tx, ty, tz;
        if (!entity_pos(env, c, ent.get(), tx, ty, tz)) continue;

        double ax, ay, az;
        compute_aim_point(c, px, py, pz, tx, ty, tz, ax, ay, az);

        // Reach is measured to the BOX, the way the server measures it. Using
        // the aim point here would reject targets that are genuinely in range
        // whenever they sit above or below us.
        const double dist = eye_to_hitbox_distance(px, py, pz, tx, ty, tz);
        if (dist > range) continue;

        const double ang = angle_to_point(yaw, pitch, px, py, pz, ax, ay, az);
        if (ang > half_angle) continue;

        // A hard ceiling under the FOV setting, kept as an invariant rather
        // than a preference: the yaw fold in update_aim only stays continuous
        // while the offset is clear of the +/-180 seam. Let the aim reach 180
        // and the fold itself becomes the 360 step it exists to prevent.
        if (ang > MAX_RETENTION_ANGLE) continue;

        const float hp = entity_health(env, c, ent.get());

        // Pinned to Vape's default, Distance.
        const bool better = !have_best || dist < best.distance;
        if (better) {
            if (best.ent) env->DeleteLocalRef(best.ent);
            best.ent      = env->NewLocalRef(ent.get());
            best.distance = dist;
            best.angle    = ang;
            best.health   = hp;
            have_best     = true;
        }
    }

    return have_best ? best.ent : nullptr;
}

// â”€â”€ the controller â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// SilentAura.updateAim(), transcribed. Reads as a PI controller whose error is
// damped by the PREVIOUS step â€” that feedback is what produces overshoot and
// settle instead of a snap, and it is the part a naive lerp aimbot lacks.
static void update_aim(JNIEnv* env, const tb::jni_cache& c, jobject player,
                       jobject target, double px, double py, double pz,
                       float real_yaw, float real_pitch)
{
    double tx, ty, tz;
    if (!entity_pos(env, c, target, tx, ty, tz)) return;

    g_jitter_x.update();
    g_jitter_z.update();
    g_jitter_pitch.update();

    // Always on. An exact aim is the single easiest thing to pick out of a
    // crowd, so the drift is part of the controller rather than a toggle.
    const double jx = g_jitter_x.value();
    const double jz = g_jitter_z.value();
    const double jp = g_jitter_pitch.value();

    double ax, ay, az;
    compute_aim_point(c, px, py, pz, tx, ty, tz, ax, ay, az);

    // Target motion, from our own previous-tick snapshot rather than the
    // entity's prevX/prevZ fields â€” same quantity, no extra mappings.
    double tmx = 0.0, tmz = 0.0;
    if (g_have_prev_target_pos) { tmx = ax - g_prev_tx; tmz = az - g_prev_tz; }
    g_prev_tx = ax; g_prev_tz = az; g_have_prev_target_pos = true;
    const double target_speed = std::sqrt(tmx * tmx + tmz * tmz);

    // Player speed, same trick.
    double pmx = 0.0, pmy = 0.0, pmz = 0.0;
    if (g_have_prev_player_pos) { pmx = px - g_prev_px; pmy = py - g_prev_py; pmz = pz - g_prev_pz; }
    const double player_speed = std::sqrt(pmx * pmx + pmz * pmz);

    const double jitter_ax = ax + jx * (1.0 + target_speed);
    const double jitter_az = az + jz * (1.0 + target_speed);

    // Vape aims at the upper body but never above the player's own eye line â€”
    // that is what keeps the pitch from pinning to the head at close range.
    const double eye_y = py + EYE_HEIGHT;
    const double jitter_ay = (eye_y < ay)
        ? ay + jp * 0.5
        : std::min(eye_y, ty + PLAYER_HEIGHT) - 0.275 + jp;

    const double ddx = jitter_ax - px;
    const double ddy = jitter_ay - eye_y;
    const double ddz = jitter_az - pz;
    const double horiz = std::sqrt(ddx * ddx + ddz * ddz);
    const double dist  = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);

    const float desired_yaw   = static_cast<float>(std::atan2(-ddx, ddz) * kDeg);
    const float desired_pitch = static_cast<float>(-std::atan2(ddy, horiz) * kDeg);

    float yaw_error   = wrap180(desired_yaw   - g_managed_yaw);
    float pitch_error = wrap180(desired_pitch - g_managed_pitch);

    float prev_yaw_step   = g_managed_yaw   - g_prev_managed_yaw;
    const float prev_pitch_step = g_managed_pitch - g_prev_managed_pitch;

    constexpr float kIntegrationStep = 0.05f;

    const bool yaw_same_dir =
        (yaw_error > 0.f) == (prev_yaw_step > 0.f) && prev_yaw_step != 0.f;

    float pitch_p = 0.45f * g_pitch_p_scale;
    float pitch_i = 0.91f * g_pitch_i_scale;
    float yaw_p   = (g_on_target ? 0.05f : 0.10f) * g_yaw_p_scale;
    float yaw_i   = 0.33f * g_yaw_i_scale;

    // Airborne: widen the pitch error a little at random. A perfectly steady
    // pitch while the body is moving vertically is not something a hand does.
    if (std::fabs(pmy) > 0.1) pitch_error *= static_cast<float>(1.0 + urand(0.0, 0.32));

    if (yaw_same_dir && std::fabs(yaw_error) < 20.f) {
        yaw_p *= 2.5f;
        prev_yaw_step *= static_cast<float>(1.0 + std::min(target_speed + player_speed, 0.25));
    }

    // Very close in, scale the correction down â€” otherwise the required angular
    // rate explodes as distance goes to zero and the aim visibly snaps.
    if (dist < 0.8) {
        const double s = dist / 0.8;
        pitch_error *= static_cast<float>(s * s);
        yaw_error   *= static_cast<float>(s);
    }

    const float pitch_ctrl = pitch_error - prev_pitch_step
                           + prev_yaw_step * kIntegrationStep * (urand(0.0, 1.0) >= 0.5 ? -1.f : 1.f);
    const float yaw_ctrl   = yaw_error - prev_yaw_step;

    g_pitch_integral += pitch_ctrl * kIntegrationStep;
    g_yaw_integral   += yaw_ctrl   * kIntegrationStep;

    const float pitch_adj = pitch_p * pitch_ctrl + pitch_i * g_pitch_integral;
    float       yaw_adj   = yaw_p   * yaw_ctrl   + yaw_i   * g_yaw_integral;

    // A target behind you is a re-acquire, not a correction. Dumping the
    // integral stops the wind-up that would otherwise fling the aim past it.
    if (std::fabs(yaw_error) > 120.f) { g_yaw_integral = 0.f; yaw_adj = 0.f; }

    const float want_yaw   = g_managed_yaw   + yaw_error + yaw_adj / 3.f;
    const float want_pitch = std::clamp(g_managed_pitch + pitch_adj, -90.f, 90.f);

    // Aim speed caps how far the managed rotation may travel in one tick. Vape
    // spends this budget across ~50 sub-steps inside its rotation controller;
    // at 20 Hz a single clamped step is the same trajectory, sampled coarser.
    // Approach as a FRACTION of what is left, never as a fixed step.
    //
    // The previous version clamped the move to a constant ceiling, so a wide
    // swing produced the identical delta tick after tick until it arrived.
    // Identical consecutive deltas ARE Vulcan's "Aim (Constant)", and a run of
    // identical deltas is a straight line, which is "Aim (Linear)". The clamp
    // was the flag.
    //
    // A proportional approach decays instead: 42, 12, 4, 1 ... no two ticks
    // alike, and the curve is nothing like a line. The fraction is re-drawn
    // every tick and the safety ceiling is randomised too, so even a swing wide
    // enough to hit the cap does not repeat a value.
    const float rem_yaw   = wrap180(want_yaw - g_managed_yaw);
    const float rem_pitch = want_pitch - g_managed_pitch;

    const double frac_y = 0.70 + urand(-0.15, 0.15);
    const double frac_p = 0.70 + urand(-0.15, 0.15);
    const float  cap    = static_cast<float>((3.0 + AIM_SPEED * 5.7)
                                             * (0.85 + urand(0.0, 0.30)));

    float dyaw   = static_cast<float>(rem_yaw   * frac_y);
    float dpitch = static_cast<float>(rem_pitch * frac_p);
    dyaw   = std::clamp(dyaw,   -cap, cap);
    dpitch = std::clamp(dpitch, -cap, cap);

    g_prev_managed_yaw   = g_managed_yaw;
    g_prev_managed_pitch = g_managed_pitch;

    // Accumulate UNWRAPPED. This is what AimModulo360 is looking for.
    //
    // Entity.yaw in Minecraft is unbounded: changeLookDirection just adds to
    // it, so a client that has been spinning sends 730.5 or -1240.0 and the
    // value drifts further from zero the longer the session runs. Nothing in
    // vanilla ever folds it back into [-180, 180] â€” the server wraps it for
    // its own use, but the number on the wire is raw.
    //
    // Wrapping the accumulator here made every packet land inside one turn,
    // which is a signature no real client produces. The errors above are still
    // wrapped, because a shortest-path delta is what the controller needs; it
    // is only the running value that has to stay in the player's own frame.
    g_managed_yaw   = g_managed_yaw + dyaw;
    g_managed_pitch = std::clamp(g_managed_pitch + dpitch, -90.f, 90.f);

    // Re-express the result in the PLAYER'S frame every tick.
    //
    // This is the AimModulo360 fix. The managed yaw is unbounded and follows
    // the target, so an enemy that circles you winds it a whole revolution away
    // from a view that never moved. Nothing looks wrong while the spoof is up --
    // the DIRECTION is right -- but the number on the wire is 360 out, and the
    // moment the spoof stops the sent yaw steps by exactly that. The check
    // reports the excess as its VL, which is why the numbers came back as
    // 0.5999999999999996 and 0.42800000000000119: deltas of ~360.6 and ~360.43.
    //
    // Folding here costs nothing because adding or subtracting a whole turn
    // does not change where the player is looking, and because the retention
    // cap below keeps the offset far from the +/-180 seam, the fold is a no-op
    // in the steady state and can never itself produce a 360 step. real_yaw is
    // itself unbounded, so the result stays unbounded exactly like vanilla's.
    g_managed_yaw = real_yaw + wrap180(g_managed_yaw - real_yaw);

    // "On target" drops the yaw proportional gain next tick, which is what
    // makes the aim settle instead of hunting once it has arrived -- and it is
    // also the gate that decides when the target may be handed to the attack
    // path. Both want the same answer: would the server accept a hit right now?
    //
    // The reach used HERE is deliberately tighter than the one used to pick a
    // target. Vanilla physically cannot attack past its own 3.0 raycast, but
    // this module publishes the entity and the hook spoofs crosshairTarget past
    // that raycast entirely -- so it CAN ask for a hit at 3.05 that no real
    // client would ever produce. Add the distance the target drifts between
    // this pump and the moment the server evaluates the attack, and Grim
    // measures something like 3.10. Engaging out to the full range while only
    // allowing hits comfortably inside it keeps the tracking without ever
    // asking for a hit vanilla could not have made.
    //
    // Judged on the rotation that was actually SENT last tick -- after the
    // divergence cap and the lattice snap -- because that is what the server
    // has when it evaluates the next attack. Judging the controller's internal
    // target instead would claim a lock the server never saw.
    const float judge_yaw   = g_have_sent ? g_sent_yaw   : g_managed_yaw;
    const float judge_pitch = g_have_sent ? g_sent_pitch : g_managed_pitch;
    const double lock_reach = attack_range() - LOCK_REACH_MARGIN;

    g_on_target = rotation_hits_target(px, py, pz, judge_yaw, judge_pitch,
                                       tx, ty, tz, lock_reach);

    // Does the player's OWN rotation already land on the hitbox? If so there is
    // nothing to correct, and apply_rotation sends their real angles untouched.
    g_player_already_on = rotation_hits_target(px, py, pz, real_yaw, real_pitch,
                                               tx, ty, tz, lock_reach);
}

// â”€â”€ rotation application â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Turn the player MODEL toward the aim, so the rotation is visible in third
// person (F5) without the first-person camera moving.
//
// This works because bodyYaw / headYaw are NOT the fields the camera reads.
// The camera orbits off Entity.yaw/pitch; the model is drawn from bodyYaw and
// headYaw, which exist precisely so a body can face one way while the head
// looks another. Writing them is pure client render state â€” nothing here is
// ever serialised into a packet, so it changes what YOU see and nothing that
// the server or other players see.
//
// The prev* pair is written alongside the live one. Rendering interpolates
// prev -> current by the frame's tick delta, so leaving prev behind would make
// the model lerp from its old angle every tick â€” a visible stutter instead of
// a turn.
static void apply_visible_rotation(JNIEnv* env, const tb::jni_cache& c, jobject player,
                                   bool active, float real_yaw)
{
    if (!c.living_body_yaw_fid && !c.living_head_yaw_fid) return;

    // When the hook is live it writes these itself, from inside the tick and
    // before the frame is drawn â€” which is the only placement that does not
    // shake. Writing them from here as well would put us back in the 20 Hz
    // fight with vanilla's own body-yaw update. Only the release path below
    // still runs, to hand the model back when the target is dropped.
    if (active && tb::silent_rotation_available()) return;

    // With no target the model goes back to following the real view, so the
    // body does not stay locked at the last aim angle.
    const float yaw = active ? g_managed_yaw : real_yaw;

    if (c.living_body_yaw_fid) {
        env->SetFloatField(player, c.living_body_yaw_fid, yaw);
        if (c.living_prev_body_yaw_fid)
            env->SetFloatField(player, c.living_prev_body_yaw_fid, yaw);
    }
    if (c.living_head_yaw_fid) {
        env->SetFloatField(player, c.living_head_yaw_fid, yaw);
        if (c.living_prev_head_yaw_fid)
            env->SetFloatField(player, c.living_prev_head_yaw_fid, yaw);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// â”€â”€ rotation quantisation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Vulcan's "Aim Rotation" and Grim's GCD checks both key on the same property:
// a real mouse cannot produce arbitrary angles.
//
// Vanilla turns raw mouse counts into degrees through
//     yaw += dx * (sens * 0.6 + 0.2)^3 * 8 * 0.15
// where dx is an integer count. Every rotation delta a human produces is
// therefore an exact multiple of one constant, and consecutive deltas share it
// as a divisor. An aimbot that writes atan2 output writes angles off that
// lattice, and the greatest common divisor of its deltas collapses toward zero
// â€” which is precisely what the check measures.
//
// Rather than reading the sensitivity option (an awkward SimpleOption chain
// that would be another mapping guess), the divisor is LEARNED from the
// player's own rotation. They are aiming manually the whole time this module
// runs, so their real deltas are a free, always-correct sample of their own
// lattice.
static float g_prev_real_yaw   = 0.f;
static float g_prev_real_pitch = 0.f;
static bool  g_have_prev_real  = false;
// NOT seeded with a guess. Seeding these with the default sensitivity's 0.15
// looked reasonable and made things worse: unless the player happens to run
// that exact sensitivity, every angle sent then sat on a lattice that was
// consistently WRONG -- a second, foreign quantisation running alongside the
// player's own, which is easier to spot than no quantisation at all. Vulcan's
// Aim (Rotation) appeared on the build that introduced it.
//
// Zero means snap_to_lattice passes the value through untouched, which is the
// honest state when we do not know the lattice.
static float g_gcd_yaw   = 0.f;
static float g_gcd_pitch = 0.f;
static float g_cand_yaw   = 0.f;    // shadow being re-learned this window
static float g_cand_pitch = 0.f;

// Solves the lattice by CONSENSUS instead of estimating it.
//
// This matters more than anything else in the file, and it took far too long to
// see. A real client's yaw is always spawn + n*gcd -- a discrete lattice, on
// every packet, without exception, because the only thing that ever moves it is
// an integer count of mouse motion. A value produced by atan2 is an arbitrary
// real. So every spoofed packet sent while the lattice is unknown is provably
// off-lattice, and the greatest common divisor of the player's rotation deltas
// collapses. That is exactly what a rotation/GCD check measures, and it fires
// on every single tick -- no amount of shaping the trajectory can hide it.
//
// Two earlier attempts failed for numerical reasons: a bare Euclidean fmod loop
// collapses to ~0 under float error, and the smallest observed delta is only
// the true lattice if the player once moved exactly one count inside a 50 ms
// window. Both were approximations of the right question, which is simply:
//
//     what is the LARGEST value that divides every delta we have seen?
//
// Asked directly, with a tolerance, over a buffer of real samples, it is exact
// and -- more importantly -- CHECKABLE. The agreement fraction says whether the
// answer can be trusted, and until it can, nothing is spoofed at all: an
// unassisted tick costs a miss, an off-lattice tick costs a flag.
static constexpr int  LATTICE_SAMPLES = 96;
static constexpr int  LATTICE_MIN_N   = 24;

struct LatticeSolver {
    float samples[LATTICE_SAMPLES] = { 0.f };
    int   count = 0;
    int   write = 0;
    float value = 0.f;
    bool  solved = false;

    void push(float d)
    {
        if (!(d > 1e-3f && d < 45.f)) return;      // stillness, dust, teleports
        samples[write] = d;
        write = (write + 1) % LATTICE_SAMPLES;
        if (count < LATTICE_SAMPLES) ++count;
    }

    // Largest divisor that explains at least 95% of the samples.
    bool solve()
    {
        if (count < LATTICE_MIN_N) return false;

        float smallest = 1e9f;
        for (int i = 0; i < count; ++i) smallest = std::min(smallest, samples[i]);
        if (!(smallest > 1e-3f)) return false;

        // k ascending walks candidates from largest to smallest, so the first
        // one that fits IS the largest that fits.
        for (int k = 1; k <= 24; ++k) {
            const float g = smallest / static_cast<float>(k);
            if (g < 5e-3f) break;                  // below any real sensitivity

            const float tol = std::max(2e-3f, g * 0.06f);
            int ok = 0;
            for (int i = 0; i < count; ++i) {
                float r = std::fmod(samples[i], g);
                if (r > g * 0.5f) r = g - r;       // distance to nearest multiple
                if (r <= tol) ++ok;
            }
            if (ok * 100 >= count * 95) { value = g; solved = true; return true; }
        }
        return false;
    }
};

static LatticeSolver g_lat_yaw;
static LatticeSolver g_lat_pitch;

// The divisor is estimated as the SMALLEST non-zero delta seen, then refined.
//
// The Euclidean version was the reason the quantisation never actually did
// anything. Run on floats it collapses: fmod(0.15000001f, 0.15f) is about 1e-8,
// the next round divides by that, and within a handful of samples the estimate
// is below any sane floor. It then failed the plausibility guard and
// snap_to_lattice returned its input untouched -- so every angle this module
// sent was off the player's mouse lattice, which is exactly what the aim checks
// are built to notice.
//
// The minimum is robust and, more importantly, SAFE when it is wrong: the
// smallest step a mouse can make is one count, so the minimum is either the
// true lattice or an integer multiple of it -- and a multiple of the lattice is
// still on the lattice. Over-estimating costs a little precision, never
// correctness.
static int g_gcd_samples = 0;

static void observe_real_rotation(float yaw, float pitch)
{
    if (g_have_prev_real) {
        const float dy = std::fabs(yaw   - g_prev_real_yaw);
        const float dp = std::fabs(pitch - g_prev_real_pitch);

        // Ignore stillness, float dust, and huge jumps -- a teleport or a
        // wrap is not a mouse movement.
        g_lat_yaw.push(dy);
        g_lat_pitch.push(dp);

        // Re-solve once a second. Cheap, and it follows a sensitivity change
        // as the old samples roll out of the buffer.
        if (++g_gcd_samples >= 20) {
            g_gcd_samples = 0;
            const bool had = g_lat_yaw.solved;
            g_lat_yaw.solve();
            g_lat_pitch.solve();
            g_gcd_yaw   = g_lat_yaw.solved   ? g_lat_yaw.value   : 0.f;
            g_gcd_pitch = g_lat_pitch.solved ? g_lat_pitch.value : g_gcd_yaw;
            if (!had && g_lat_yaw.solved)
                VLOG_INFO("silentaim",
                          "rotation lattice solved: yaw={} pitch={} (from {} samples)"
                          " -- silent rotation is now on-lattice",
                          g_gcd_yaw, g_gcd_pitch, g_lat_yaw.count);
        }
    }
    g_prev_real_yaw   = yaw;
    g_prev_real_pitch = pitch;
    g_have_prev_real  = true;
}

// Snap an angle onto the player's own lattice. The OFFSET from the real angle
// is what gets quantised, not the absolute value: the real angle is already on
// the lattice, so real + (k * gcd) stays on it. Quantising the absolute value
// would not.
// Deliberately NO synthetic noise on the output any more.
//
// Successive builds layered a dither on the rounding, an overshoot on the final
// approach, and an Ornstein-Uhlenbeck tremor on the sent angle, each aimed at
// making the delta sequence look less machine-made. None of them moved the
// flags, and the reason is that the trajectory was never the trigger: the build
// that flagged nothing had a CONSTANT clamped step and Vape's plain linear
// jitter ramp, and what actually broke it was fixing the aim point and the
// range check -- which made the module engage far more often and for far
// longer. What Vulcan's aim family reacts to here is the VOLUME of synthetic
// rotation, not its shape.
//
// So the noise is gone. It never earned its place, it cost aim precision, and
// artificial randomness carries a signature of its own.
static float snap_to_lattice(float target, float real, float gcd)
{
    // Plausible mouse lattices run from about 0.02 deg (very low sensitivity)
    // to a couple of degrees (very high). The old ceiling of 1.0 rejected
    // perfectly normal high-sensitivity setups on top of the estimator bug.
    if (!(gcd > 1e-3f && gcd < 3.f)) gcd = 0.15f;   // safe fallback to 0.15 deg (100% sens)

    const float offset = target - real;
    return real + std::round(offset / gcd) * gcd;
}

// Shared with the action lease. The old quantize_action_angles returned false
// until the solver had 24 accepted mouse windows, which is exactly why Auto
// Crystal sat on "Rotating" forever for a player standing still: nothing ever
// fed the solver. The lease now snaps with whatever lattice is known -- the
// sensitivity option first, this solver second, nothing third -- and acts
// regardless. Grim has no GCD check; the snap is for other anticheats only.
void feed_lattice_observer(float real_yaw, float real_pitch)
{
    static unsigned long long observed = 0;
    const auto now = GetTickCount64();
    if (now - observed < 50) return;
    observed = now;
    observe_real_rotation(real_yaw, real_pitch);
}

float observed_lattice_deg()
{
    return g_gcd_yaw > 0.f ? g_gcd_yaw : 0.f;
}

float snap_action_angle(float target, float real, float lattice)
{
    return snap_to_lattice(target, real, lattice);
}

// Publishes the rotation for the packet. Nothing here ever writes
// Entity.yaw/pitch â€” those are what the CAMERA renders from, and this module
// does not move the camera under any circumstance.
//
// There used to be a "Client" fallback that wrote them when the hook was
// unavailable. That is exactly the aim-assist behaviour this module must not
// have, so it is gone: when the hook is not verified the rotation simply does
// not reach the server, and the module still redirects hits and still turns
// the head in F5.
static void apply_rotation(JNIEnv*, const tb::jni_cache&, jobject,
                           float real_yaw, float real_pitch)
{
    if (!tb::silent_rotation_available()) {
        tb::set_managed_rotation(false, 0.f, 0.f, 0.f);
        return;
    }

    // An action rotation (a lease, or the easing that walks it home) owns the
    // wire: park on the real view. set_managed_rotation drops the publish
    // while that holds anyway; parking the controller too matters for the
    // moment the easing lands. Left running, it kept integrating toward its
    // target with nothing going out, and the first flying after the landing
    // then carried the aura's accumulated angle -- a step of up to the
    // retention cap on a wire whose previous deltas were the easing's small
    // ones. Parked, the aura resumes from where the player is looking, which
    // is also where the easing just delivered the wire.
    if (tb::action_rotation_active()) {
        g_managed_yaw   = real_yaw;
        g_managed_pitch = real_pitch;
        g_sent_yaw      = real_yaw;
        g_sent_pitch    = real_pitch;
        g_have_sent     = true;
        g_yaw_integral  = 0.f;
        g_pitch_integral = 0.f;
        return;
    }

    // No verified lattice -- send NOTHING.
    //
    // Spoofing here would put an arbitrary real number on the wire where a real
    // client can only ever put spawn + n*gcd. That is not a subtle statistical
    // tell; it is a per-packet certainty, and it is what a rotation/GCD check
    // exists to find. Until the solver has an answer it can defend, the module
    // stays out of the way. It converges within a second or two of the player
    // touching the mouse.
    if (!(g_gcd_yaw > 0.f)) {
        float fb = silentaim::mouse_lattice_deg();
        if (fb > 1e-3f && fb < 3.f) {
            g_gcd_yaw   = fb;
            g_gcd_pitch = fb;
        } else {
            g_managed_yaw   = real_yaw;
            g_managed_pitch = real_pitch;
            g_sent_yaw      = real_yaw;
            g_sent_pitch    = real_pitch;
            g_have_sent     = true;
            tb::set_managed_rotation(false, 0.f, 0.f, 0.f);
            return;
        }
    }

    // The player is already looking at the target -- send NOTHING.
    //
    // This costs nothing and is the single cheapest reduction in synthetic
    // rotation available. Whenever their own crosshair already lands on the
    // hitbox, the module has nothing to contribute, so the wire carries their
    // genuine mouse data and there is simply no artificial rotation for an aim
    // check to analyse. Over a fight that is a large fraction of the ticks.
    //
    // The controller is kept parked on the real rotation while this holds, so
    // the moment their aim drifts off the correction starts from where they
    // actually are and there is no step.
    if (g_player_already_on) {
        g_managed_yaw   = real_yaw;
        g_managed_pitch = real_pitch;
        g_sent_yaw      = real_yaw;
        g_sent_pitch    = real_pitch;
        g_have_sent     = true;
        tb::set_managed_rotation(false, 0.f, 0.f, 0.f);
        return;
    }

    float yaw   = snap_to_lattice(g_managed_yaw,   real_yaw,   g_gcd_yaw);
    float pitch = snap_to_lattice(g_managed_pitch, real_pitch, g_gcd_pitch);

    // Remember what actually went out. The lock test has to judge the rotation
    // the SERVER has, not the controller's internal wish -- after the cap and
    // the snap those are no longer the same thing.
    g_sent_yaw   = yaw;
    g_sent_pitch = pitch;
    g_have_sent  = true;

    // Publish how far this sits from the player's own view. The attack hook
    // uses it to tell a spoof that changes what the server sees from one that
    // is close enough that the player's own click is still honest -- most of
    // the release is the latter.
    const float offset = std::max(std::fabs(wrap180(yaw - real_yaw)),
                                  std::fabs(pitch - real_pitch));

    // The hook swaps the fields in for the duration of sendMovementPackets and
    // puts them straight back, so the spoof never outlives that one call.
    tb::set_managed_rotation(true, yaw, pitch, offset);
}

// Hands the rotation back over a few ticks instead of cutting it dead.
//
// Every way this module can stand down -- target lost, module switched off,
// ClickGui opened, a screen opened, the weapon put away -- used to end the
// spoof on a single tick. The server then saw the yaw jump straight from the
// aim angle to the player's view, and after a long engagement against someone
// circling them that step can be most of a full turn, which is the shape
// AimModulo360 names. Routing every one of them through here means the sent
// rotation has no discontinuities at all.
static void disengage(JNIEnv* env, const tb::jni_cache& c, jobject player,
                      float real_yaw, float real_pitch)
{
    if (g_has_target) {
        g_has_target = false;
        clear_target(env);
        ::dt::modules::triggerbot_hook::set_silentaim_target(env, nullptr);
        g_release_ticks = RELEASE_TICKS;
    }
    if (g_release_ticks <= 0) return;

    --g_release_ticks;
    // Raw difference for the same reason the test below uses it: a wrapped
    // delta hides a whole-turn offset instead of unwinding it.
    g_managed_yaw   += (real_yaw   - g_managed_yaw)   * 0.55f;
    g_managed_pitch += (real_pitch - g_managed_pitch) * 0.55f;

    // Converge on the NUMBER, not the direction. wrap180 was the bug here: with
    // a full turn of accumulated offset it reported zero error, the release
    // declared itself finished on its first tick, and the spoof stopped with
    // the sent yaw still 360 out. The fold in update_aim now keeps the offset
    // small, so the raw difference is the honest measure.
    if (std::fabs(real_yaw   - g_managed_yaw)   < 0.25f &&
        std::fabs(real_pitch - g_managed_pitch) < 0.25f)
        g_release_ticks = 0;

    apply_rotation(env, c, player, real_yaw, real_pitch);
    apply_visible_rotation(env, c, player, true, real_yaw);

    if (g_release_ticks == 0) {
        reset();
        // Hand the model back on the same step, or the body stays frozen at
        // the last aim angle.
        apply_visible_rotation(env, c, player, false, real_yaw);
    }
}

// â”€â”€ the pump â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void pump(float delta_ms)
{
    if (tb::g_shutting_down.load(std::memory_order_acquire)) return;

    // Freecam detaches the camera from the player, so the crosshair target no
    // longer comes from the player's view: while it is on this module behaves
    // exactly as if it were switched off (same reset / hand-back path).
    const bool module_on = tb::settings::enabled.load(std::memory_order_relaxed)
                        && !vgui::bridge::isEnabled("Freecam");

    // Throttle to the world-tick rate. The controller's gains are expressed
    // per tick; running it at frame rate would multiply every one of them.
    static double accum_ms = 0.0;
    accum_ms += (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.0;
    if (accum_ms < TICK_MS) return;
    accum_ms -= TICK_MS;
    if (accum_ms > TICK_MS) accum_ms = TICK_MS;

    const auto& c = tb::get_cache();
    if (!c.initialized || !c.minecraft_class || !c.mc_player_fid) return;

    JNIEnv* env = dt::jvm::thisThreadEnv();
    if (!env) return;

    // Switched off with nothing left to hand back: genuinely nothing to do.
    // Every other case still needs the player, because standing down is now a
    // few ticks of easing rather than an instant cut.
    if (!module_on && !g_has_target && g_release_ticks <= 0) return;

    jfieldID inst = env->GetStaticFieldID(c.minecraft_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!inst) return;

    LocalRef mc(env, env->GetStaticObjectField(c.minecraft_class, inst));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;

    LocalRef player(env, env->GetObjectField(mc.get(), c.mc_player_fid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    // No player to write to at all â€” nothing can be eased, so drop it flat.
    if (!player) { if (g_has_target || g_release_ticks > 0) reset(); return; }

    bool screen_open = false;
    if (c.mc_current_screen_fid) {
        LocalRef screen(env, env->GetObjectField(mc.get(), c.mc_current_screen_fid));
        if (env->ExceptionCheck()) env->ExceptionClear();
        screen_open = (screen.get() != nullptr);
    }

    double px, py, pz;
    if (!entity_pos(env, c, player.get(), px, py, pz)) return;

    float real_yaw   = c.entity_yaw_fid   ? env->GetFloatField(player.get(), c.entity_yaw_fid)   : 0.f;
    float real_pitch = c.entity_pitch_fid ? env->GetFloatField(player.get(), c.entity_pitch_fid) : 0.f;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // Keep learning the player's rotation lattice whether or not we have a
    // target â€” the sample is only valid while THEY are driving the mouse, so
    // the idle stretches between fights are the best data we get.
    observe_real_rotation(real_yaw, real_pitch);

    // Every stand-down goes through the same easing. The ClickGui needs its own
    // test because the panel is an ImGui surface, not a Minecraft Screen, so
    // currentScreen is null while it is up and a null check misses it.
    const bool stand_down = !module_on
                         || tb::settings::overlay_open.load(std::memory_order_relaxed)
                         || screen_open
                         || !holding_weapon(env, c, player.get());
    if (stand_down) {
        if (screen_open) {
            // Vape zeroes the integrals here rather than carrying stale
            // wind-up into the moment the screen closes.
            g_yaw_integral   = 0.f;
            g_pitch_integral = 0.f;
        }
        disengage(env, c, player.get(), real_yaw, real_pitch);
        g_prev_px = px; g_prev_py = py; g_prev_pz = pz; g_have_prev_player_pos = true;
        return;
    }

    LocalRef target(env, pick_target(env, c, mc.get(), player.get(),
                                     px, py, pz, real_yaw, real_pitch));

    if (!target) {
        disengage(env, c, player.get(), real_yaw, real_pitch);
        g_prev_px = px; g_prev_py = py; g_prev_pz = pz; g_have_prev_player_pos = true;
        return;
    }

    // Target switch (or first acquisition): re-seed the managed rotation from
    // where the player is actually looking, and re-roll the gain profile.
    const bool changed = !g_target_ref ||
        env->IsSameObject(g_target_ref, target.get()) != JNI_TRUE;
    if (changed) {
        clear_target(env);
        g_target_ref = env->NewGlobalRef(target.get());

        // Re-anchor to the player's view WITHOUT a full-turn jump.
        //
        // Assigning real_yaw outright looks harmless, but the managed yaw is
        // unbounded and follows the target: an enemy that circles you winds it
        // a whole revolution away from a stationary view. Snapping back then
        // moves the SENT yaw by ~360 in one tick even though the direction is
        // unchanged -- and a 360 step is precisely what AimModulo360 names.
        // Picking the representative of real_yaw nearest the current value is
        // the same direction with no step at all.
        const bool re_anchor = g_has_target || g_release_ticks > 0;
        g_managed_yaw   = re_anchor ? g_managed_yaw + wrap180(real_yaw - g_managed_yaw)
                                    : real_yaw;
        g_managed_pitch      = real_pitch;
        g_prev_managed_yaw   = g_managed_yaw;
        g_prev_managed_pitch = real_pitch;
        g_release_ticks      = 0;

        // A new target starts unlocked no matter how well aimed we were at the
        // last one. Carrying the count over is exactly how a click during a
        // switch got redirected before the rotation had moved.
        g_locked_ticks       = 0;
        g_yaw_integral       = 0.f;
        g_pitch_integral     = 0.f;
        g_on_target          = false;
        g_have_prev_target_pos = false;
        randomize_gain_factors();
    }
    g_has_target = true;

    update_aim(env, c, player.get(), target.get(), px, py, pz, real_yaw, real_pitch);
    apply_rotation(env, c, player.get(), real_yaw, real_pitch);
    apply_visible_rotation(env, c, player.get(), true, real_yaw);

    if (g_on_target) ++g_locked_ticks;
    else             g_locked_ticks = 0;

    // Hand the target to the triggerbot hook -- but ONLY once the rotation has
    // caught up to it.
    //
    // That hook spoofs MinecraftClient.crosshairTarget inside doAttack, which
    // is what makes a click land on this entity with a vanilla swing. Publishing
    // it the instant the target is chosen is what produced the rare Hitbox
    // flags: on a switch between two players standing together, the new one was
    // published immediately while the sent rotation still pointed at the old
    // one, so a click in that window asked the server to accept an attack on an
    // entity it did not believe we were looking at.
    //
    // Withholding it during the swing costs a click that would have missed
    // anyway, and buys never asking the server for something indefensible.
    const bool locked = g_locked_ticks >= REQUIRED_LOCK_TICKS;
    ::dt::modules::triggerbot_hook::set_silentaim_target(
        env, locked ? target.get() : nullptr);

    g_prev_px = px; g_prev_py = py; g_prev_pz = pz; g_have_prev_player_pos = true;
}

} // namespace dt::modules::silentaim_tick
