// Backtrack keeps a short visual history without delaying network packets.
// A historical hit is eligible only when the current target box also intersects
// the player's ray within normal reach, before the blocking vanilla hit.
// Teleports, stale samples, focus loss and Freecam invalidate interaction.
// These checks reduce stale/invalid interactions; they do not establish
// compatibility with any particular server anticheat.
//
// Names verified in tools/genmaps/.cache/<ver>/yarn_merged.tiny for 1.21,
// 1.21.4 and 1.21.11 (identical intermediary ids throughout, except where a
// fallback is listed):
//   MinecraftClient class_310: instance field_1700, world field_1687,
//     player field_1724, currentScreen field_1755, crosshairTarget field_1765
//     (Lclass_239;), targetedEntity field_1692, isOnThread method_18854,
//     getRenderTickCounter method_61966 (1.21/1.21.1: method_60646)
//   RenderTickCounter class_9779: getTickProgress method_60637 (Z)F
//   ClientWorld class_638: getEntityById method_8469 (I)
//   HitResult class_239: getType method_17783, getPos method_17784
//   EntityHitResult class_3966: <init>(Lclass_1297;Lclass_243;)V,
//     getEntity method_17782
//   Entity class_1297: getId method_5628, isAlive method_5805, isSpectator
//     method_7325, getX/Y/Z method_23317/23318/23321, getBoundingBox
//     method_5829, getLerpedPos method_30950 (F), getCameraPosVec method_5836
//     (F), getRotationVec method_5828 (F), isSneaking method_18276,
//     pitch field_5965, lastPitch field_6004
//   LivingEntity class_1309: bodyYaw field_6283, lastBodyYaw field_6220,
//     headYaw field_6241, lastHeadYaw field_6259, limbAnimator field_42108,
//     getMainHandStack method_6047
//   LimbAnimator class_8080: progress method_48572 (F)F, amplitude
//     method_48570 (F)F
//   PlayerEntity class_1657: getEntityInteractionRange method_55755 ()D
//   Box class_238: field_1323/1322/1321 min xyz, field_1320/1325/1324 max xyz
//   Vec3d class_243: field_1352/1351/1350, <init>(DDD)V
//   ItemStack class_1799: getItem method_7909; Item class_1792:
//     getTranslationKey method_7876

#include "backtrack.h"
#include "core/module/Module.h"
#include "core/social/EntityFriends.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "modules/render/world_overlay_draw_scope.h"
#include "modules/render/world_overlay_view.h"
#include "modules/render/chams/chams_pose.h"
#include "modules/combat/triggerbot/triggerbot_hook.h"
#include "util/Log.h"
#include "vgui/bridge.h"
#include "imgui.h"
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace vanta::modules::backtrack {
namespace {

using vanta::jvm::LocalObject;
using vanta::jvm::LocalClass;
using Millis = std::int64_t;

constexpr float kDeg = 3.14159265358979323846f / 180.f;

// How long a target survives after your last hit on it (body or ghost). Long
// enough to bridge the gap between two hits of a fight, short enough that a
// player you stopped hitting does not keep a trail.
constexpr Millis kRetainMs = 2000;
// History kept beyond the delay, so the ghost can always be interpolated.
constexpr Millis kHistorySlackMs = 300;
// A target further than this from you is not a fight.
constexpr double kTrackRangeSq = 8.0 * 8.0;

Millis now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Vec { double x = 0, y = 0, z = 0; };
struct Box { Vec min, max; bool valid = false; };

struct Options {
    bool enabled = false, weapon = false, ghost = true;
    int  delay  = 100;   // ms the ghost trails the body
    int  visual = 0;     // 0 box, 1 wireframe
    RGBA border{0.31f, 0.57f, 0.85f, 0.9f};
    RGBA fill{0.31f, 0.57f, 0.85f, 0.16f};
};

// One frame of the target as it was drawn — the ghost is one of these, or the
// blend of two neighbours.
struct Sample {
    Millis t = 0;
    Box    box;
    chams::pose::pose_input pose;
};

struct Cache {
    bool tried = false, ready = false;
    std::vector<jclass> classes;
    jclass mc_class = nullptr, player_class = nullptr, living_class = nullptr;
    jclass hit_class = nullptr, hitresult_class = nullptr, vec_class = nullptr;
    jfieldID instance = nullptr, world = nullptr, player = nullptr, screen = nullptr;
    jfieldID crosshair = nullptr, targeted = nullptr;
    jmethodID on_thread = nullptr, rtc = nullptr, rtc_progress = nullptr;
    jmethodID world_entity = nullptr;
    jmethodID id = nullptr, alive = nullptr, spectator = nullptr;
    jmethodID x = nullptr, y = nullptr, z = nullptr, get_box = nullptr;
    jmethodID lerped_pos = nullptr, camera_pos = nullptr, rotation_vec = nullptr, sneak = nullptr;
    jfieldID pitch = nullptr, last_pitch = nullptr;
    jfieldID body_yaw = nullptr, last_body_yaw = nullptr, head_yaw = nullptr, last_head_yaw = nullptr;
    jfieldID limb = nullptr;
    jmethodID limb_progress = nullptr, limb_amplitude = nullptr;
    jmethodID reach = nullptr;
    std::array<jfieldID, 6> box{};
    jfieldID vx = nullptr, vy = nullptr, vz = nullptr;
    jmethodID vec_ctor = nullptr;
    jmethodID hit_ctor = nullptr, hit_entity = nullptr, hit_type = nullptr, hit_pos = nullptr;
    jmethodID enum_ordinal = nullptr;
    jmethodID hand = nullptr, item = nullptr, item_key = nullptr;
} c;

// Pump (GameLoopEvent, game thread) and render (ImGui pass, same thread today)
// share everything below; ghost_hit_distance is called by the triggerbot in
// between. Recursive because drop() is reached from both locked paths.
std::recursive_mutex mx;
Options options;

jobject target = nullptr, target_world = nullptr;
int     target_id = -1;
Millis  last_hit = 0;                 // last click that landed on the body or the ghost
bool    lmb_was_down = false;         // left button state last frame, for the press edge
unsigned long long bot_attacks = 0;   // triggerbot dispatch counter last frame
std::deque<Sample> history;           // ascending t
Sample  ghost;
bool    ghost_valid = false;
bool    ghost_aimed = false;          // this frame the crosshair ray hits the ghost
double  ghost_hit_dist = -1.0;
const char* state = "Waiting for target";

// ── JNI helpers ──────────────────────────────────────────────────────────────
bool clear_error(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}
jclass klass(JNIEnv* env, const char* name) {
    LocalClass local(env, vanta::jvm::findClassAnyLoader(env, name));
    if (clear_error(env) || !local) return nullptr;
    auto result = static_cast<jclass>(env->NewGlobalRef(local.get()));
    if (result) c.classes.push_back(result);
    return result;
}
jmethodID method(JNIEnv* env, jclass owner, const char* name, const char* sig) {
    if (!owner) return nullptr;
    auto result = env->GetMethodID(owner, name, sig);
    clear_error(env);
    return result;
}
jfieldID field(JNIEnv* env, jclass owner, const char* name, const char* sig) {
    if (!owner) return nullptr;
    auto result = env->GetFieldID(owner, name, sig);
    clear_error(env);
    return result;
}
LocalObject minecraft(JNIEnv* env) {
    return LocalObject(env, c.instance ? env->GetStaticObjectField(c.mc_class, c.instance) : nullptr);
}
bool main_thread(JNIEnv* env, jobject mc) {
    if (!mc || !c.on_thread) return false;
    const bool result = env->CallBooleanMethod(mc, c.on_thread) == JNI_TRUE;
    return !clear_error(env) && result;
}
Vec vec_of(JNIEnv* env, jobject v) {
    return {env->GetDoubleField(v, c.vx), env->GetDoubleField(v, c.vy), env->GetDoubleField(v, c.vz)};
}
Vec position(JNIEnv* env, jobject entity) {
    return {env->CallDoubleMethod(entity, c.x), env->CallDoubleMethod(entity, c.y),
        env->CallDoubleMethod(entity, c.z)};
}
double distance_squared(const Vec& a, const Vec& b) {
    const double x = a.x-b.x, y = a.y-b.y, z = a.z-b.z;
    return x*x+y*y+z*z;
}
bool finite(const Vec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
void release(JNIEnv* env, jobject& ref) {
    if (ref) env->DeleteGlobalRef(ref);
    ref = nullptr;
}
float lerp_angle(float a, float b, float t) {
    return a + std::remainder(b - a, 360.f) * t;
}

// Render tick progress, so samples are taken where the model is DRAWN rather
// than where the tick left it; the ghost then moves as smoothly as the body.
float tick_progress(JNIEnv* env, jobject mc) {
    if (!c.rtc || !c.rtc_progress) return 1.f;
    LocalObject counter(env, env->CallObjectMethod(mc, c.rtc));
    if (clear_error(env) || !counter) return 1.f;
    const float tp = env->CallFloatMethod(counter.get(), c.rtc_progress, JNI_TRUE);
    if (clear_error(env) || !std::isfinite(tp)) return 1.f;
    return std::clamp(tp, 0.f, 1.f);
}

bool weapon_held(JNIEnv* env, jobject player) {
    if (!c.hand || !c.item || !c.item_key) return true;
    LocalObject stack(env, env->CallObjectMethod(player, c.hand));
    if (clear_error(env) || !stack) return false;
    LocalObject item(env, env->CallObjectMethod(stack.get(), c.item));
    if (clear_error(env) || !item) return false;
    vanta::jvm::LocalString key(env, static_cast<jstring>(env->CallObjectMethod(item.get(), c.item_key)));
    if (clear_error(env) || !key) return false;
    const std::string name = vanta::jvm::toStdString(env, key.get());
    return name.ends_with("_sword") || name.ends_with("_axe")
        || name.ends_with(".trident") || name.ends_with(".mace");
}

bool valid_target(JNIEnv* env, jobject entity, jobject player) {
    if (!entity || env->IsSameObject(entity, player) || !env->IsInstanceOf(entity, c.player_class)) return false;
    const bool alive = env->CallBooleanMethod(entity, c.alive) == JNI_TRUE;
    if (clear_error(env) || !alive) return false;
    const bool spectator = env->CallBooleanMethod(entity, c.spectator) == JNI_TRUE;
    if (clear_error(env) || spectator) return false;
    const double distance = distance_squared(position(env, entity), position(env, player));
    if (clear_error(env) || !std::isfinite(distance) || distance > kTrackRangeSq) return false;
    return !vanta::social::isFriend(env, entity);
}

// ── target lifecycle ─────────────────────────────────────────────────────────
void drop(JNIEnv* env, const char* reason) {
    if (target) VLOG_DEBUG("backtrack", "Target dropped: {} (id={})", reason, target_id);
    release(env, target);
    release(env, target_world);
    target_id = -1;
    history.clear();
    ghost_valid = false;
    ghost_aimed = false;
    ghost_hit_dist = -1.0;
    state = reason;
}
void acquire(JNIEnv* env, jobject entity, jobject world) {
    drop(env, "Switched target");
    target = env->NewGlobalRef(entity);
    target_world = env->NewGlobalRef(world);
    target_id = env->CallIntMethod(entity, c.id);
    if (clear_error(env) || !target || !target_world) { drop(env, "Acquire failed"); return; }
    last_hit = now_ms();
    state = "Tracking";
}

// ── sampling ─────────────────────────────────────────────────────────────────
// The target as drawn this frame: its hitbox shifted onto the interpolated
// render position, and the pose the model is wearing.
bool sample_target(JNIEnv* env, jobject entity, float tp, Sample& out) {
    LocalObject box(env, env->CallObjectMethod(entity, c.get_box));
    if (clear_error(env) || !box) return false;
    Vec bmin{env->GetDoubleField(box.get(), c.box[0]), env->GetDoubleField(box.get(), c.box[1]),
        env->GetDoubleField(box.get(), c.box[2])};
    Vec bmax{env->GetDoubleField(box.get(), c.box[3]), env->GetDoubleField(box.get(), c.box[4]),
        env->GetDoubleField(box.get(), c.box[5])};
    const Vec pos = position(env, entity);
    Vec drawn = pos;
    if (c.lerped_pos) {
        LocalObject lp(env, env->CallObjectMethod(entity, c.lerped_pos, tp));
        if (!clear_error(env) && lp) drawn = vec_of(env, lp.get());
    }
    if (clear_error(env)) return false;
    const Vec d{drawn.x-pos.x, drawn.y-pos.y, drawn.z-pos.z};
    out.box.min = {bmin.x+d.x, bmin.y+d.y, bmin.z+d.z};
    out.box.max = {bmax.x+d.x, bmax.y+d.y, bmax.z+d.z};
    out.box.valid = finite(out.box.min) && finite(out.box.max);

    chams::pose::pose_input p{};
    float body = c.body_yaw ? env->GetFloatField(entity, c.body_yaw) : 0.f;
    if (c.body_yaw && c.last_body_yaw)
        body = lerp_angle(env->GetFloatField(entity, c.last_body_yaw), body, tp);
    float head = c.head_yaw ? env->GetFloatField(entity, c.head_yaw) : body;
    if (c.head_yaw && c.last_head_yaw)
        head = lerp_angle(env->GetFloatField(entity, c.last_head_yaw), head, tp);
    float pitch = c.pitch ? env->GetFloatField(entity, c.pitch) : 0.f;
    if (c.pitch && c.last_pitch) {
        const float last = env->GetFloatField(entity, c.last_pitch);
        pitch = last + (pitch - last) * tp;
    }
    p.body_yaw_deg     = body;
    p.head_yaw_rel_deg = std::remainder(head - body, 360.f);
    p.head_pitch_deg   = pitch;
    if (c.sneak) p.sneaking = env->CallBooleanMethod(entity, c.sneak) == JNI_TRUE;
    if (c.limb && c.limb_progress && c.limb_amplitude) {
        LocalObject animator(env, env->GetObjectField(entity, c.limb));
        if (!clear_error(env) && animator) {
            p.limb_swing = env->CallFloatMethod(animator.get(), c.limb_progress, tp);
            // Vanilla feeds the model min(speed, 1); unclamped a sprinter's
            // limbs flail past the skin.
            p.limb_amplitude = std::clamp(env->CallFloatMethod(animator.get(), c.limb_amplitude, tp), 0.f, 1.f);
        }
    }
    if (clear_error(env)) p = {};
    out.pose = p;
    out.t = now_ms();
    return out.box.valid;
}

// The ghost: the sample `delay` ms ago, blended between its two neighbours so
// it moves as smoothly as the body did. Younger history than the delay means
// the oldest sample — which is how the ghost first emerges from the body and
// then drops back to its full distance.
bool ghost_at(Millis tq, Sample& out) {
    if (history.empty()) return false;
    if (tq <= history.front().t) { out = history.front(); return out.box.valid; }
    if (tq >= history.back().t)  { out = history.back();  return out.box.valid; }
    auto it = std::upper_bound(history.begin(), history.end(), tq,
        [](Millis v, const Sample& s) { return v < s.t; });
    const Sample& b = *it;
    const Sample& a = *(it - 1);
    const double span = static_cast<double>(b.t - a.t);
    const float  f = span > 0.0 ? static_cast<float>(static_cast<double>(tq - a.t) / span) : 0.f;
    const auto mix = [f](double p, double q) { return p + (q - p) * f; };
    out = f < 0.5f ? a : b;   // discrete state (sneaking) from the nearer sample
    out.t = tq;
    out.box.min = {mix(a.box.min.x, b.box.min.x), mix(a.box.min.y, b.box.min.y), mix(a.box.min.z, b.box.min.z)};
    out.box.max = {mix(a.box.max.x, b.box.max.x), mix(a.box.max.y, b.box.max.y), mix(a.box.max.z, b.box.max.z)};
    out.box.valid = a.box.valid && b.box.valid;
    out.pose.body_yaw_deg     = lerp_angle(a.pose.body_yaw_deg, b.pose.body_yaw_deg, f);
    out.pose.head_yaw_rel_deg = lerp_angle(a.pose.head_yaw_rel_deg, b.pose.head_yaw_rel_deg, f);
    out.pose.head_pitch_deg   = a.pose.head_pitch_deg + (b.pose.head_pitch_deg - a.pose.head_pitch_deg) * f;
    out.pose.limb_swing       = a.pose.limb_swing + (b.pose.limb_swing - a.pose.limb_swing) * f;
    out.pose.limb_amplitude   = a.pose.limb_amplitude + (b.pose.limb_amplitude - a.pose.limb_amplitude) * f;
    return out.box.valid;
}

// ── the crosshair ────────────────────────────────────────────────────────────
// Eye position and look vector as vanilla's own pick uses them.
bool read_ray(JNIEnv* env, jobject player, float tp, Vec& eye, Vec& dir) {
    LocalObject e(env, env->CallObjectMethod(player, c.camera_pos, tp));
    if (clear_error(env) || !e) return false;
    LocalObject d(env, env->CallObjectMethod(player, c.rotation_vec, tp));
    if (clear_error(env) || !d) return false;
    eye = vec_of(env, e.get());
    dir = vec_of(env, d.get());
    if (clear_error(env) || !finite(eye) || !finite(dir)) return false;
    const double len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (!(len > 1e-9)) return false;
    dir = {dir.x/len, dir.y/len, dir.z/len};
    return true;
}

// Slab test. Writes the entry distance (0 when the eye is inside the box).
bool ray_box(const Vec& o, const Vec& d, const Box& b, double& t) {
    double t_near = -std::numeric_limits<double>::infinity();
    double t_far  =  std::numeric_limits<double>::infinity();
    const double os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z};
    const double lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-9) {
            if (os[i] < lo[i] || os[i] > hi[i]) return false;
            continue;
        }
        double t1 = (lo[i] - os[i]) / ds[i];
        double t2 = (hi[i] - os[i]) / ds[i];
        if (t1 > t2) std::swap(t1, t2);
        t_near = std::max(t_near, t1);
        t_far  = std::min(t_far, t2);
        if (t_near > t_far) return false;
    }
    if (t_far < 0.0) return false;
    t = t_near < 0.0 ? 0.0 : t_near;
    return true;
}

// Point vanilla's pick at the target where the ray met the ghost. Vanilla
// overwrites the field at the start of the next world render, and the click
// (handleInputEvents → doAttack) runs before that, so one write per frame is
// exactly one frame of "the ghost is the target".
bool spoof_crosshair(JNIEnv* env, jobject mc, const Vec& p) {
    LocalObject pos(env, env->NewObject(c.vec_class, c.vec_ctor, p.x, p.y, p.z));
    if (clear_error(env) || !pos) return false;
    LocalObject hit(env, env->NewObject(c.hit_class, c.hit_ctor, target, pos.get()));
    if (clear_error(env) || !hit) return false;
    env->SetObjectField(mc, c.crosshair, hit.get());
    if (c.targeted) env->SetObjectField(mc, c.targeted, target);
    return !clear_error(env);
}

// ── JNI resolution ───────────────────────────────────────────────────────────
bool install(JNIEnv* env) {
    if (c.tried) return c.ready;
    c.tried = true;
    c.mc_class        = klass(env, "net/minecraft/class_310");
    c.player_class    = klass(env, "net/minecraft/class_1657");
    c.living_class    = klass(env, "net/minecraft/class_1309");
    c.hit_class       = klass(env, "net/minecraft/class_3966");
    c.hitresult_class = klass(env, "net/minecraft/class_239");
    c.vec_class       = klass(env, "net/minecraft/class_243");
    auto entity   = klass(env, "net/minecraft/class_1297");
    auto box      = klass(env, "net/minecraft/class_238");
    auto world    = klass(env, "net/minecraft/class_638");
    auto stack    = klass(env, "net/minecraft/class_1799");
    auto item     = klass(env, "net/minecraft/class_1792");
    auto animator = klass(env, "net/minecraft/class_8080");
    auto counter  = klass(env, "net/minecraft/class_9779");
    if (!c.mc_class || !c.player_class || !c.living_class || !c.hit_class || !c.hitresult_class
        || !c.vec_class || !entity || !box || !world) {
        VLOG_WARN("backtrack", "Required Fabric intermediary classes unavailable; module inactive");
        return false;
    }
    c.instance = env->GetStaticFieldID(c.mc_class, "field_1700", "Lnet/minecraft/class_310;");
    clear_error(env);
    c.world     = field(env, c.mc_class, "field_1687", "Lnet/minecraft/class_638;");
    c.player    = field(env, c.mc_class, "field_1724", "Lnet/minecraft/class_746;");
    c.screen    = field(env, c.mc_class, "field_1755", "Lnet/minecraft/class_437;");
    c.crosshair = field(env, c.mc_class, "field_1765", "Lnet/minecraft/class_239;");
    c.targeted  = field(env, c.mc_class, "field_1692", "Lnet/minecraft/class_1297;");
    c.on_thread = method(env, c.mc_class, "method_18854", "()Z");
    c.rtc = method(env, c.mc_class, "method_61966", "()Lnet/minecraft/class_9779;");
    if (!c.rtc) c.rtc = method(env, c.mc_class, "method_60646", "()Lnet/minecraft/class_9779;");
    c.rtc_progress = method(env, counter, "method_60637", "(Z)F");
    c.world_entity = method(env, world, "method_8469", "(I)Lnet/minecraft/class_1297;");
    c.id        = method(env, entity, "method_5628", "()I");
    c.alive     = method(env, entity, "method_5805", "()Z");
    c.spectator = method(env, entity, "method_7325", "()Z");
    c.x = method(env, entity, "method_23317", "()D");
    c.y = method(env, entity, "method_23318", "()D");
    c.z = method(env, entity, "method_23321", "()D");
    c.get_box      = method(env, entity, "method_5829", "()Lnet/minecraft/class_238;");
    c.lerped_pos   = method(env, entity, "method_30950", "(F)Lnet/minecraft/class_243;");
    c.camera_pos   = method(env, entity, "method_5836", "(F)Lnet/minecraft/class_243;");
    c.rotation_vec = method(env, entity, "method_5828", "(F)Lnet/minecraft/class_243;");
    c.sneak        = method(env, entity, "method_18276", "()Z");
    c.pitch        = field(env, entity, "field_5965", "F");
    c.last_pitch   = field(env, entity, "field_6004", "F");
    c.body_yaw      = field(env, c.living_class, "field_6283", "F");
    c.last_body_yaw = field(env, c.living_class, "field_6220", "F");
    c.head_yaw      = field(env, c.living_class, "field_6241", "F");
    c.last_head_yaw = field(env, c.living_class, "field_6259", "F");
    c.limb          = field(env, c.living_class, "field_42108", "Lnet/minecraft/class_8080;");
    c.limb_progress  = method(env, animator, "method_48572", "(F)F");
    c.limb_amplitude = method(env, animator, "method_48570", "(F)F");
    c.reach = method(env, c.player_class, "method_55755", "()D");
    const char* bounds[] = {"field_1323", "field_1322", "field_1321", "field_1320", "field_1325", "field_1324"};
    for (int i = 0; i < 6; ++i) c.box[i] = field(env, box, bounds[i], "D");
    c.vx = field(env, c.vec_class, "field_1352", "D");
    c.vy = field(env, c.vec_class, "field_1351", "D");
    c.vz = field(env, c.vec_class, "field_1350", "D");
    c.vec_ctor   = method(env, c.vec_class, "<init>", "(DDD)V");
    c.hit_ctor   = method(env, c.hit_class, "<init>", "(Lnet/minecraft/class_1297;Lnet/minecraft/class_243;)V");
    c.hit_entity = method(env, c.hit_class, "method_17782", "()Lnet/minecraft/class_1297;");
    c.hit_type   = method(env, c.hitresult_class, "method_17783", "()Lnet/minecraft/class_239$class_240;");
    c.hit_pos    = method(env, c.hitresult_class, "method_17784", "()Lnet/minecraft/class_243;");
    if (jclass en = env->FindClass("java/lang/Enum")) {
        c.enum_ordinal = method(env, en, "ordinal", "()I");
        env->DeleteLocalRef(en);
    }
    clear_error(env);
    c.hand     = method(env, c.living_class, "method_6047", "()Lnet/minecraft/class_1799;");
    c.item     = method(env, stack, "method_7909", "()Lnet/minecraft/class_1792;");
    c.item_key = method(env, item, "method_7876", "()Ljava/lang/String;");

    bool resolved = c.instance && c.world && c.player && c.screen && c.crosshair && c.on_thread
        && c.world_entity && c.id && c.alive && c.spectator && c.x && c.y && c.z && c.get_box
        && c.camera_pos && c.rotation_vec && c.vx && c.vy && c.vz && c.vec_ctor
        && c.hit_ctor && c.hit_entity;
    for (auto b : c.box) resolved = resolved && b;
    VLOG_INFO("backtrack", "cache: core={} tickProgress={} lerpedPos={} pose={} limbs={} dist={} hitType={} weapon={} -> {}",
        resolved, (c.rtc && c.rtc_progress), c.lerped_pos != nullptr,
        (c.body_yaw && c.last_body_yaw && c.head_yaw && c.pitch && c.last_pitch),
        (c.limb && c.limb_progress && c.limb_amplitude), c.reach != nullptr,
        (c.hit_type && c.hit_pos && c.enum_ordinal), (c.hand && c.item && c.item_key),
        resolved ? "ready" : "INACTIVE (required mapping missing)");
    c.ready = resolved;
    return resolved;
}

// ── per-frame pump (GameLoopEvent, game thread, before the triggerbot) ───────
void pump(JNIEnv* env) {
    // A click this frame landed on whatever the crosshair held LAST frame
    // (handleInputEvents runs before the render that refreshes the pick), so
    // remember whether that was the ghost before clearing it.
    const bool aimed_prev = ghost_aimed;
    ghost_aimed = false;
    ghost_hit_dist = -1.0;
    if (!c.ready) { state = "Mappings unavailable (see log)"; return; }
    LocalObject mc = minecraft(env);
    if (!mc || !main_thread(env, mc.get())) return;
    LocalObject player(env, env->GetObjectField(mc.get(), c.player));
    LocalObject world(env, env->GetObjectField(mc.get(), c.world));
    if (clear_error(env) || !player || !world) { drop(env, "No world"); return; }

    if (!options.enabled) { drop(env, "Disabled"); return; }
    if (vanta::vgui::bridge::isOverlayVisible()) { drop(env, "GUI open"); return; }
    {
        LocalObject screen(env, env->GetObjectField(mc.get(), c.screen));
        if (clear_error(env) || screen) { drop(env, "Screen open"); return; }
    }
    const bool alive = env->CallBooleanMethod(player.get(), c.alive) == JNI_TRUE;
    if (clear_error(env) || !alive) { drop(env, "Player not alive"); return; }
    if (options.weapon && !weapon_held(env, player.get())) { drop(env, "Weapon required"); return; }

    const Millis now = now_ms();
    const float  tp  = tick_progress(env, mc.get());

    // Only a HIT arms the ghost: a left-click, yours or the triggerbot's, with
    // the game in the foreground. Aiming at a player does nothing on its own —
    // this is what used to make the ghost "switch itself on".
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    const bool  focused = foreground_pid == GetCurrentProcessId();
    if (!focused || vanta::vgui::bridge::isEnabled("Freecam")) { drop(env, "Input unavailable"); return; }
    const SHORT lmb     = GetAsyncKeyState(VK_LBUTTON);
    const bool  down    = (lmb & 0x8000) != 0;
    // The press edge, or a press shorter than a frame (bit 0: pressed since
    // the last call). The triggerbot's synthetic click can be either, so its
    // own dispatch counter is a hit as well.
    const bool  clicked = (down && !lmb_was_down) || (lmb & 1) != 0;
    lmb_was_down = down;
    const auto  attacks = vanta::modules::triggerbot_hook::debug_attack_dispatch_count();
    const bool  bot_hit = attacks != bot_attacks;
    bot_attacks = attacks;
    const bool  pressed = focused && (clicked || bot_hit);

    if (target && !env->IsSameObject(world.get(), target_world)) drop(env, "World changed");
    if (target) {
        LocalObject current(env, env->CallObjectMethod(target_world, c.world_entity, target_id));
        if (clear_error(env) || !current || !env->IsSameObject(current.get(), target)
            || !valid_target(env, target, player.get()))
            drop(env, "Target gone / out of range");
    }

    Vec eye, dir;
    const bool ray_ok = read_ray(env, player.get(), tp, eye, dir);

    // Vanilla's own pick. A body under the crosshair is never overridden by the
    // ghost; a CLICK on it arms (or switches) the target. A block in front of
    // the ghost wins too.
    bool   vanilla_entity = false;
    double block_dist = std::numeric_limits<double>::infinity();
    {
        LocalObject hit(env, env->GetObjectField(mc.get(), c.crosshair));
        if (!clear_error(env) && hit) {
            if (env->IsInstanceOf(hit.get(), c.hit_class) == JNI_TRUE) {
                LocalObject ent(env, env->CallObjectMethod(hit.get(), c.hit_entity));
                if (!clear_error(env) && ent) {
                    vanilla_entity = true;
                    if (pressed && valid_target(env, ent.get(), player.get())) {
                        if (!target || !env->IsSameObject(ent.get(), target)) acquire(env, ent.get(), world.get());
                        last_hit = now;
                    }
                }
            } else if (c.hit_type && c.enum_ordinal && c.hit_pos && ray_ok) {
                LocalObject type(env, env->CallObjectMethod(hit.get(), c.hit_type));
                const jint ordinal = (!clear_error(env) && type) ? env->CallIntMethod(type.get(), c.enum_ordinal) : 0;
                if (!clear_error(env) && ordinal == 1) {   // HitResult.Type.BLOCK
                    LocalObject pos(env, env->CallObjectMethod(hit.get(), c.hit_pos));
                    if (!clear_error(env) && pos) {
                        const Vec p = vec_of(env, pos.get());
                        if (!clear_error(env) && finite(p)) block_dist = std::sqrt(distance_squared(p, eye));
                    }
                }
            }
        }
    }

    // A click while the crosshair sat on the ghost is a hit on the target too.
    if (pressed && aimed_prev && target) last_hit = now;
    if (!target) { state = "Waiting for a hit"; return; }
    if (now - last_hit > kRetainMs) { drop(env, "No hit for a while"); return; }

    Sample s;
    if (sample_target(env, target, tp, s)) {
        if (!history.empty() && (s.t - history.back().t > 250
            || distance_squared(s.box.min, history.back().box.min) > 4.0)) {
            history.clear(); // Do not interpolate through teleports or stalls.
        }
        history.push_back(s);
        while (history.size() > 512) history.pop_front();
        const Millis oldest = now - options.delay - kHistorySlackMs;
        while (history.size() > 1 && history.front().t < oldest) history.pop_front();
    }
    ghost_valid = ghost_at(now - options.delay, ghost);
    state = "Tracking";

    if (!ghost_valid || !ray_ok || vanilla_entity) return;
    double reach = 2.975;
    if (c.reach) {
        const double r = env->CallDoubleMethod(player.get(), c.reach);
        if (!clear_error(env) && std::isfinite(r) && r > 0.0) reach = std::min(r, 2.975);
    }
    // Historical visuals must not authorize attacks outside the server's real
    // unshifted bounding box or previous-tick rotation (Spartan HIT_REACH).
    LocalObject real_box_obj(env, env->CallObjectMethod(target, c.get_box));
    if (clear_error(env) || !real_box_obj) return;
    constexpr double kInset = 0.025;
    Box real_box{
        {env->GetDoubleField(real_box_obj.get(), c.box[0]) + kInset,
         env->GetDoubleField(real_box_obj.get(), c.box[1]) + kInset,
         env->GetDoubleField(real_box_obj.get(), c.box[2]) + kInset},
        {env->GetDoubleField(real_box_obj.get(), c.box[3]) - kInset,
         env->GetDoubleField(real_box_obj.get(), c.box[4]) - kInset,
         env->GetDoubleField(real_box_obj.get(), c.box[5]) - kInset},
        true
    };
    if (clear_error(env) || !finite(real_box.min) || !finite(real_box.max)) return;

    Vec eye0, dir0, eye1, dir1;
    double t0 = 0.0, t1 = 0.0;
    if (!read_ray(env, player.get(), 0.f, eye0, dir0)
        || !read_ray(env, player.get(), 1.f, eye1, dir1)
        || !ray_box(eye0, dir0, real_box, t0) || t0 > reach || t0 >= block_dist
        || !ray_box(eye1, dir1, real_box, t1) || t1 > reach || t1 >= block_dist) return;

    double t = 0.0;
    if (!ray_box(eye, dir, ghost.box, t) || t > reach || t >= block_dist) return;
    const Vec hit{eye.x + dir.x*t, eye.y + dir.y*t, eye.z + dir.z*t};
    if (!spoof_crosshair(env, mc.get(), hit)) return;
    ghost_aimed = true;
    ghost_hit_dist = t;
    state = "Crosshair on ghost";
}

void shutdown() {
    std::lock_guard lock(mx);
    options.enabled = false;
    vanta::jvm::JniScope scope;
    if (!scope) return;
    JNIEnv* env = scope.get();
    drop(env, "Shutdown");
    for (auto cls : c.classes) env->DeleteGlobalRef(cls);
    c = {};
}

// ── drawing ──────────────────────────────────────────────────────────────────
ImU32 colour(RGBA v, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.r, v.g, v.b, alpha));
}

// Sneaking. pose::build_pose reproduces the ANGLES vanilla applies while
// sneaking but not the part TRANSLATIONS that go with them; same table and
// frame as outline.cpp (entity-local blocks, +z forward, scaled with the model).
constexpr float k_sneak_dy[chams::pose::kPartCount] = { -0.2625f, -0.2f, -0.2f, -0.2f, -0.0125f, -0.0125f };
constexpr float k_sneak_dz[chams::pose::kPartCount] = {  0.f,      0.f,   0.f,   0.f,  -0.25f,   -0.25f   };

// A translucent silhouette plus twelve edges, the way BlockESP draws a block:
// filling the six faces separately covers every pixel twice and draws the
// internal seams, which is what made the old box look wrong at every angle.
void draw_box(ImDrawList* dl, const ImVec2 (&pt)[8], ImU32 border, ImU32 fill, float thickness) {
    ImVec2 pts[8];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        bool dup = false;
        for (int j = 0; j < n; ++j)
            if (std::fabs(pt[i].x - pts[j].x) < 0.5f && std::fabs(pt[i].y - pts[j].y) < 0.5f) { dup = true; break; }
        if (!dup) pts[n++] = pt[i];
    }
    if (n >= 3 && ((fill >> IM_COL32_A_SHIFT) & 0xFF) != 0) {
        std::sort(pts, pts + n, [](const ImVec2& a, const ImVec2& b) {
            return a.x < b.x || (a.x == b.x && a.y < b.y);
        });
        const auto cross = [](const ImVec2& o, const ImVec2& a, const ImVec2& b) {
            return (double)(a.x - o.x) * (double)(b.y - o.y) - (double)(a.y - o.y) * (double)(b.x - o.x);
        };
        ImVec2 hull[18];
        int k = 0;
        for (int i = 0; i < n; ++i) {
            while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
            hull[k++] = pts[i];
        }
        const int lower = k + 1;
        for (int i = n - 2; i >= 0; --i) {
            while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
            hull[k++] = pts[i];
        }
        --k;
        double area2 = 0.0;
        for (int i = 0, j = k - 1; i < k; j = i++)
            area2 += (double)hull[j].x * hull[i].y - (double)hull[i].x * hull[j].y;
        if (k >= 3 && std::fabs(area2) * 0.5 >= 1.0) dl->AddConvexPolyFilled(hull, k, fill);
    }
    // Corner index: bit0 x, bit1 y, bit2 z. Twelve edges differ in one bit.
    for (int a = 0; a < 8; ++a)
        for (int bit = 1; bit < 8; bit <<= 1)
            if (!(a & bit)) {
                const int b = a | bit;
                const float ex = pt[b].x - pt[a].x, ey = pt[b].y - pt[a].y;
                if (ex * ex + ey * ey < 0.25f) continue;   // collapsed edge → blob, skip
                dl->AddLine(pt[a], pt[b], border, thickness);
            }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
double ghost_hit_distance(JNIEnv* env, jobject entity) {
    std::lock_guard lock(mx);
    if (!env || !entity || !target || !ghost_aimed) return -1.0;
    return env->IsSameObject(entity, target) == JNI_TRUE ? ghost_hit_dist : -1.0;
}

void render(int width, int height) {
    Sample  g;
    Options opts;
    bool    aimed = false;
    {
        std::lock_guard lock(mx);
        if (!options.enabled || !options.ghost || !ghost_valid || !target) return;
        g = ghost; opts = options; aimed = ghost_aimed;
    }
    if (width < 2 || height < 2) return;
    if (vanta::vgui::bridge::isOverlayVisible()) return;

    namespace cap = vanta::modules::hitfx::capture;
    if (!cap::is_installed() && !cap::install()) return;
    vanta::jvm::JniScope scope;
    if (!scope) return;
    cap::set_capture_enabled(true);
    cap::capture_now(scope.get());
    hitfx::render_frame frame{};
    if (!hitfx::latest_frame(frame) || !frame.valid || frame.viewport_w < 1 || frame.viewport_h < 1) return;
    // The capture carries the camera rotation only; fold in the walking bob and
    // hurt tilt the world was drawn with, or the ghost swims off the target with
    // every step YOU take (see world_overlay_view.h).
    overlay_view::apply_view_bob(scope.get(), frame);

    auto* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    WorldOverlayDrawScope draw_scope(dl);
    // REPLACE the inherited clip rect, never intersect with it. The foreground
    // list is seeded every frame with the ClickGui's 1120x700 canvas rect
    // (gl_hook pins io.DisplaySize to the canvas), and intersecting kept it:
    // on any bigger window everything drawn right of x=1120 or below y=700 was
    // scissored away — the box vanished when you looked sideways or jumped,
    // and the wireframe lost its legs. BlockESP had the same fix.
    dl->PushClipRect(ImVec2(0.f, 0.f), ImVec2(static_cast<float>(width), static_cast<float>(height)), false);

    const float vw = static_cast<float>(frame.viewport_w), vh = static_cast<float>(frame.viewport_h);
    const float sx = static_cast<float>(width) / vw, sy = static_cast<float>(height) / vh;
    const auto project = [&](double wx, double wy, double wz, ImVec2& out) -> bool {
        float x, y, depth;
        if (!hitfx::world_to_screen(frame, wx, wy, wz, x, y, depth)) return false;
        // A corner just past the near plane projects to a finite but absurd
        // coordinate (clip_w ~ 0), and one such vertex drags an edge across the
        // whole screen. Treat it as unusable rather than draw the spike.
        if (std::fabs(x) > vw * 1.5f || std::fabs(y) > vh * 1.5f) return false;
        out = ImVec2(x * sx, y * sy);
        return true;
    };

    // The border lights up while the crosshair is on the ghost: that is the
    // frame in which a click lands on it.
    const ImU32 border = colour(opts.border, aimed ? 1.f : opts.border.a);
    const float thickness = aimed ? 2.f : 1.f;
    const Box& box = g.box;

    if (opts.visual == 0) {
        ImVec2 pt[8];
        bool ok = true;
        for (int i = 0; i < 8 && ok; ++i)
            ok = project((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                         (i & 4) ? box.max.z : box.min.z, pt[i]);
        if (ok) draw_box(dl, pt, border, colour(opts.fill, opts.fill.a), thickness);
    } else {
        // The client's own biped geometry, posed as the target was at the time
        // of the sample, stood at the ghost's feet and turned to its body yaw.
        namespace model = chams::pose;
        model::part_def parts[model::kPartCount];
        std::copy(std::begin(model::kParts), std::end(model::kParts), parts);
        model::part_pose angles[model::kPartCount];
        model::build_pose(g.pose, parts, angles);
        const double cx = (box.min.x + box.max.x) * 0.5, cz = (box.min.z + box.max.z) * 0.5, fy = box.min.y;
        const float ct = std::cos(g.pose.body_yaw_deg * kDeg), st = std::sin(g.pose.body_yaw_deg * kDeg);
        for (int part = 0; part < model::kPartCount; ++part) {
            float local[8][3];
            model::part_corners(parts[part], angles[part], 0.f, g.pose.scale, local);
            const float sdy = g.pose.sneaking ? k_sneak_dy[part] * g.pose.scale : 0.f;
            const float sdz = g.pose.sneaking ? k_sneak_dz[part] * g.pose.scale : 0.f;
            ImVec2 pt[8];
            bool ok = true;
            for (int i = 0; i < 8 && ok; ++i) {
                // chams::pose and the PlayerESP describe the biped in frames 180
                // degrees apart about Y; flip into the ESP's frame, add the
                // sneak shift there, then turn by the body yaw — the same three
                // steps outline.cpp validated on screen.
                const float lx = -local[i][0];
                const float ly =  local[i][1] + sdy;
                const float lz = -local[i][2] + sdz;
                ok = project(cx + static_cast<double>(lx * ct - lz * st), fy + static_cast<double>(ly),
                             cz + static_cast<double>(lx * st + lz * ct), pt[i]);
            }
            if (!ok) continue;
            // part_corners index: bit2 x, bit1 y, bit0 z. Twelve edges differ in one bit.
            for (int a = 0; a < 8; ++a)
                for (int bit = 1; bit < 8; bit <<= 1)
                    if (!(a & bit)) {
                        const int b = a | bit;
                        const float ex = pt[b].x - pt[a].x, ey = pt[b].y - pt[a].y;
                        if (ex * ex + ey * ey < 0.25f) continue;
                        dl->AddLine(pt[a], pt[b], border, thickness);
                    }
        }
    }
    dl->PopClipRect();
}

// ─────────────────────────────────────────────────────────────────────────────
class Backtrack : public Module {
public:
    Backtrack() : Module("Backtrack", "Historical player position tracking with distance validation", Category::Combat) {
        registerSetting(&delay_); registerSetting(&weapon_); registerSetting(&ghost_);
        registerSetting(&visual_); registerSetting(&border_); registerSetting(&fill_);
        visual_.setVisibleIf([this] { return ghost_.value(); });
        border_.setVisibleIf([this] { return ghost_.value(); });
        fill_.setVisibleIf([this] { return ghost_.value() && visual_.index() == 0; });
    }
    ~Backtrack() override { shutdown(); }
    void onInit() override {
        // High priority on purpose: the triggerbot decides from
        // MinecraftClient.crosshairTarget in its own (Normal) GameLoop listener,
        // and this frame's ghost spoof has to be in the field before it looks.
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&) {
            std::lock_guard lock(mx);
            options.enabled = isEnabled();
            options.weapon  = weapon_.value();
            options.ghost   = ghost_.value();
            options.delay   = delay_.value();
            options.visual  = visual_.index();
            options.border  = border_.value();
            options.fill    = fill_.value();
            if (!options.enabled && !target) return;
            vanta::jvm::JniScope scope;
            if (!scope) return;
            if (!c.tried && !install(scope.get())) return;
            pump(scope.get());
        }, Priority::High);
    }
    std::string displayInfo() const override {
        std::lock_guard lock(mx);
        if (c.tried && !c.ready) return "Unavailable";
        return std::to_string(delay_.value()) + " ms";
    }
private:
    // Historical delay is visual; an attack also requires the current target
    // box to intersect the crosshair ray within the normal interaction range.
    IntSetting   delay_ {this, "Delay MS", 100, 0, 1000};
    BoolSetting  weapon_{this, "Require Weapon", false};
    BoolSetting  ghost_ {this, "Show Ghost", true};
    EnumSetting  visual_{this, "Ghost Mode", {"Box", "Wireframe"}, 0};
    ColorSetting border_{this, "Ghost Border", {0.31f, 0.57f, 0.85f, 0.9f}};
    ColorSetting fill_  {this, "Ghost Fill", {0.31f, 0.57f, 0.85f, 0.16f}};

    void onEnable() override {
        std::lock_guard lock(mx);
        options.enabled = true;
        lmb_was_down = true;
        bot_attacks = vanta::modules::triggerbot_hook::debug_attack_dispatch_count();
    }
    void onDisable() override {
        std::lock_guard lock(mx);
        options.enabled = false;
        vanta::jvm::JniScope scope;
        if (scope) drop(scope.get(), "Module disabled");
        else { history.clear(); ghost_valid = false; ghost_aimed = false; }
    }
};
VANTA_REGISTER_MODULE(Backtrack);

} // namespace vanta::modules::backtrack
