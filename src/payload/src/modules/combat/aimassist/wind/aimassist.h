#pragma once

#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace dt::modules::aimassist {

struct JniCache {
    bool initialized = false;

    jclass minecraft_class  = nullptr;
    jclass entity_class     = nullptr;
    jclass living_class     = nullptr;
    jclass player_class     = nullptr;
    jclass client_player_class = nullptr;
    jclass armor_stand_class  = nullptr;
    jclass world_class      = nullptr;
    jclass vec3d_class      = nullptr;
    jclass box_class        = nullptr;
    jclass inventory_class  = nullptr;
    jclass item_stack_class = nullptr;
    jclass item_class       = nullptr;
    jclass options_class    = nullptr;
    jclass simple_option_class = nullptr;
    jclass block_hit_class  = nullptr;
    jclass key_binding_class = nullptr;
    jclass render_tick_counter_class = nullptr;

    jmethodID mc_render_tick_counter_mid = nullptr;
    jmethodID rtc_tick_progress_mid      = nullptr;

    jfieldID  mc_instance_fid   = nullptr;
    jfieldID  mc_player_fid     = nullptr;
    jfieldID  mc_world_fid      = nullptr;
    jfieldID  mc_options_fid    = nullptr;
    jfieldID  mc_screen_fid     = nullptr;
    jfieldID  mc_crosshair_fid  = nullptr;

    jfieldID  opts_sens_fid = nullptr;

    jmethodID sopt_get_value_mid = nullptr;

    jmethodID ent_get_x_mid    = nullptr;
    jmethodID ent_get_y_mid    = nullptr;
    jmethodID ent_get_z_mid    = nullptr;
    jmethodID ent_is_alive_mid = nullptr;
    jmethodID ent_is_spectator_mid = nullptr;
    jmethodID ent_is_removed_mid   = nullptr;
    jmethodID ent_on_ground_mid = nullptr;
    jmethodID ent_invisible_mid = nullptr;
    jmethodID ent_eye_mid      = nullptr;
    jfieldID  ent_yaw_fid      = nullptr;
    jfieldID  ent_pitch_fid    = nullptr;

    jfieldID  ent_prev_yaw_fid   = nullptr;
    jfieldID  ent_prev_pitch_fid = nullptr;
    jfieldID  ent_velocity_fid = nullptr;
    jfieldID  ent_prev_x_fid   = nullptr;
    jfieldID  ent_prev_y_fid   = nullptr;
    jfieldID  ent_prev_z_fid   = nullptr;
    jfieldID  ent_bbox_fid     = nullptr;
    jmethodID ent_bbox_mid     = nullptr;

    jfieldID box_min_x_fid = nullptr;
    jfieldID box_min_y_fid = nullptr;
    jfieldID box_min_z_fid = nullptr;
    jfieldID box_max_x_fid = nullptr;
    jfieldID box_max_y_fid = nullptr;
    jfieldID box_max_z_fid = nullptr;

    jfieldID cp_render_yaw_fid       = nullptr;
    jfieldID cp_render_pitch_fid     = nullptr;
    jfieldID cp_last_render_yaw_fid  = nullptr;
    jfieldID cp_last_render_pitch_fid= nullptr;

    jfieldID vec_x_fid = nullptr;
    jfieldID vec_y_fid = nullptr;
    jfieldID vec_z_fid = nullptr;

    jmethodID living_health_mid   = nullptr;
    jmethodID living_armor_mid    = nullptr;
    jmethodID living_mainhand_mid = nullptr;

    jfieldID  player_inventory_fid = nullptr;
    jfieldID  inv_selected_fid     = nullptr;
    jmethodID inv_get_stack_mid    = nullptr;

    jmethodID stack_get_item_mid = nullptr;
    jmethodID stack_is_empty_mid = nullptr;
    jmethodID item_get_key_mid   = nullptr;

    jmethodID world_entities_mid = nullptr;

    jmethodID iterable_iterator_mid = nullptr;
    jmethodID iterator_has_next_mid = nullptr;
    jmethodID iterator_next_mid     = nullptr;

    jmethodID double_value_mid = nullptr;

    jmethodID key_translation_mid = nullptr;
    jfieldID  key_pressed_fid     = nullptr;
    jfieldID  move_key_fids[4]    = {nullptr, nullptr, nullptr, nullptr};
    bool      move_keys_resolved  = false;
};

bool ensure_cache(JNIEnv* env);
const JniCache& cache();
void drop_cache(JNIEnv* env);

struct EntPos { double x, y, z; };
EntPos entityPos(JNIEnv* env, jobject ent);
EntPos entityPrevPos(JNIEnv* env, jobject ent);

float  renderTickDelta(JNIEnv* env);

EntPos entityRenderPos(JNIEnv* env, jobject ent, float td);
struct EntVel { double x, y, z; };
EntVel entityVelocity(JNIEnv* env, jobject ent);
float  entityEyeHeight(JNIEnv* env, jobject ent);
struct Box6 { double minX, minY, minZ, maxX, maxY, maxZ; bool valid; };
Box6   entityBox(JNIEnv* env, jobject ent);
float  playerSensitivity(JNIEnv* env);
float  strafeInput(JNIEnv* env, jobject mc);

void applyTrackedMouseDelta(JNIEnv* env, jobject player, float yawDelta, float pitchDelta);

struct JavaRandom {
    uint64_t seed48 = 0;
    JavaRandom();
    void setSeed(uint64_t s);
    int    next(int bits);
    int    nextInt();
    int    nextInt(int bound);
    float  nextFloat();
    double nextDouble();
};

float  wrapAngleTo180_f(float v);
double wrapAngleTo180_d(double v);

float  rotationYawTo(double px, double pz, double tx, double tz);

int    angleToTarget(float playerYaw, double px, double pz, double tx, double tz);

double horizAngleDiff(double px, double pz, float yaw, double tx, double tz);

bool   targetOnLeft(double px, double pz, float yaw, double tx, double tz);

int    vertAngleDiff(float playerPitch, double px, double py, double pz,
                     double aimX, double aimY, double aimZ);

double clampToRange(double v, double lo, double hi);

struct Vec3 { double x, y, z; };
Vec3  closestPointOnBox(double ex, double ey, double ez, const Box6& box);

Vec3  stableClosestPointOnBox(double ex, double ey, double ez, const Box6& box);

double randomRange(JavaRandom& rnd, double lo, double hi);
int    randomExclusiveUpper(JavaRandom& rnd, int lo, int hi);

inline double nowSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline long long nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct SettingsView {
    int   mode = 0;
    bool  requireMouseDown = false;
    bool  aimVertically    = false;
    bool  strafeIncrease   = false;
    bool  checkBlockBreak  = false;
    float horizontalSpeed  = 5.f;
    float verticalSpeed    = 5.f;
    int   maxAngle         = 180;
    float distance         = 5.f;
    int   targetMode       = 0;
    int   targetArea       = 0;
    bool  players          = true;
    bool  mobs             = false;
    bool  ignoreInvisible  = false;
    bool  limitToItems     = false;
    bool  teams            = false;

    float emissionSmooth   = 5.f;

    bool  jumpCompensate   = true;

    bool  humanize         = false;

    bool  focusMode        = false;
    float focusRange       = 6.f;
};

int drainSmoothed(float& state, float pendingUnits, float smoothSetting, float dtSec);

jobject findBestTarget(JNIEnv* env, const SettingsView& sv);
bool    isValidTarget(JNIEnv* env, jobject target, const SettingsView& sv);
bool    canAim(JNIEnv* env, const SettingsView& sv, int& blockBreakCooldown);
bool    isDeadOrDying(JNIEnv* env, jobject target);
double  distanceToEntity(JNIEnv* env, jobject player, jobject target);

struct SharedState {
    std::mutex mx;

    float horizontalMouseAccumulator = 0.f;
    float verticalMouseAccumulator   = 0.f;
    int   driftX = 0;
    int   driftY = 0;

    float pendingYaw   = 0.f;
    float pendingPitch = 0.f;
    bool  yawSnapped   = false;
    bool  pitchSnapped = false;

    float smoothH = 0.f, smoothV = 0.f;
    float smoothPY = 0.f, smoothPP = 0.f;

    float cmdRateYaw = 0.f, cmdRatePitch = 0.f;
    long long cmdStampNs = 0;
    float rateFiltYaw = 0.f, rateFiltPitch = 0.f;
    float carryYaw = 0.f, carryPitch = 0.f;
    float emitRateYaw = 0.f, emitRatePitch = 0.f;

    float lockonCarryYaw = 0.f, lockonCarryPitch = 0.f;

    jobject targetRef  = nullptr;
};

namespace regular {
    void workerTick(JNIEnv* env, SharedState& sh, const SettingsView& sv,
                    int& blockBreakCooldown);
    void renderApply(JNIEnv* env, jobject player, SharedState& sh, const SettingsView& sv);
    void onDisable(JNIEnv* env, SharedState& sh);
}
namespace lockon {
    void workerTick(JNIEnv* env, SharedState& sh, const SettingsView& sv,
                    int& blockBreakCooldown);
    void renderApply(JNIEnv* env, jobject player, SharedState& sh, const SettingsView& sv);
    void onDisable(JNIEnv* env, SharedState& sh);
}

struct Runtime {
    SharedState shared;
    std::mutex control;
    SettingsView settings;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> moduleEnabled{false};

    ~Runtime() { stop(); }
    void start();
    void stop();
};

}
