// hitfx_capture_hook.cpp — no-hook JNI read of MC's real render matrices.

#include "hitfx_capture_hook.h"
#include "hitfx_render_context.h"
#include "hitfx_debug.h"

#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include "vgui/core/gl_hook.hh"
#include "util/Log.h"

#include <atomic>
#include <cmath>

namespace dt::modules::hitfx::capture {

namespace {

// ── Verified Yarn 1.21.11 symbols ────────────────────────────────────────────
constexpr const char* kMcClass       = "net/minecraft/class_310";
constexpr const char* kGameRenderer  = "net/minecraft/class_757";
constexpr const char* kCameraClass   = "net/minecraft/class_4184";
constexpr const char* kVec3dClass    = "net/minecraft/class_243";
constexpr const char* kMatrix4fClass = "org/joml/Matrix4f";
constexpr const char* kRenderTickCounter = "net/minecraft/class_9779";

std::atomic<bool>               g_ready { false };
std::atomic<bool>               g_on    { false };
std::atomic<unsigned long long> g_fires { 0 };

// MinecraftClient
jclass    g_mc_class = nullptr;
jmethodID g_mc_getInstance = nullptr;
jfieldID  g_mc_gameRenderer = nullptr;
// GameRenderer
jmethodID g_gr_getCamera = nullptr;                 // ()Camera
jmethodID g_gr_getFov = nullptr;                    // (Camera,float,boolean)float
jmethodID g_gr_getBasicProjectionMatrix = nullptr;  // (float)Matrix4f
jmethodID g_gr_getProjectionMatrix = nullptr;       // (float)Matrix4f — the WORLD one
jmethodID g_cam_getRotation = nullptr;              // ()Quaternionf — exact view rotation
jfieldID  g_q_x = nullptr, g_q_y = nullptr, g_q_z = nullptr, g_q_w = nullptr;
// RenderTickCounter — real partial-tick for the fov lerp (fixes zoom-anim lag)
jmethodID g_mc_getRenderTickCounter = nullptr;      // ()RenderTickCounter
jmethodID g_rtc_getTickProgress = nullptr;          // (boolean)float
// Camera
jfieldID  g_cam_pos = nullptr;   // Vec3d
jfieldID  g_cam_yaw = nullptr;   // float
jfieldID  g_cam_pitch = nullptr; // float
// Vec3d
jfieldID  g_vx = nullptr, g_vy = nullptr, g_vz = nullptr;
// Matrix4f m00..m33 → column-major flat[col*4+row]
jfieldID  g_mat[16] = {};

// Read a JOML Matrix4f into 16 column-major floats.
inline void read_matrix(JNIEnv* env, jobject mat, float out[16])
{
    if (!mat) { for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.f : 0.f; return; }
    for (int i = 0; i < 16; ++i) out[i] = env->GetFloatField(mat, g_mat[i]);
}

// column-major 4x4 multiply: C = A*B, index [col*4+row].
inline void mat_mul(const float* A, const float* B, float* C)
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += A[k*4+row] * B[col*4+k];
            C[col*4+row] = s;
        }
}

void set_identity(float* m) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.f : 0.f; }

// MC's first-person view: modelview = RotX(pitch) * RotY(yaw+180), applied to
// camera-relative world coords. Angles in DEGREES (Camera.pitch/yaw).
void build_modelview(float pitch_deg, float yaw_deg, float out[16])
{
    const float pr = pitch_deg * 3.14159265358979f / 180.f;
    const float yr = (yaw_deg + 180.f) * 3.14159265358979f / 180.f;
    const float cp = std::cos(pr), sp = std::sin(pr);
    const float cy = std::cos(yr), sy = std::sin(yr);

    // RotY(yr), column-major [col*4+row]
    float ry[16]; set_identity(ry);
    ry[0] = cy;  ry[8]  = sy;
    ry[2] = -sy; ry[10] = cy;
    // RotX(pr)
    float rx[16]; set_identity(rx);
    rx[5] = cp;  rx[9]  = -sp;
    rx[6] = sp;  rx[10] = cp;

    mat_mul(rx, ry, out);   // RotX * RotY
}

// Exact view rotation from the camera's own quaternion. MC builds the view by
// applying the CONJUGATE of Camera.rotation, so this reproduces the real matrix
// including any roll (hurt tilt, nausea) that pitch+yaw alone cannot express.
void build_modelview_from_quat(float qx, float qy, float qz, float qw, float out[16])
{
    qx = -qx; qy = -qy; qz = -qz;                 // conjugate
    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wx = qw*qx, wy = qw*qy, wz = qw*qz;
    set_identity(out);
    out[0]  = 1.f - 2.f*(yy+zz); out[4] = 2.f*(xy-wz);       out[8]  = 2.f*(xz+wy);
    out[1]  = 2.f*(xy+wz);       out[5] = 1.f - 2.f*(xx+zz); out[9]  = 2.f*(yz-wx);
    out[2]  = 2.f*(xz-wy);       out[6] = 2.f*(yz+wx);       out[10] = 1.f - 2.f*(xx+yy);
}

} // namespace

bool install()
{
    if (g_ready.load(std::memory_order_acquire)) return true;

    JNIEnv* env = dt::jvm::thisThreadEnv();
    if (!env) { dbg::log("capture install: no env"); return false; }

    jclass mc  = dt::jvm::findClassAnyLoader(env, kMcClass);
    jclass gr  = dt::jvm::findClassAnyLoader(env, kGameRenderer);
    jclass cam = dt::jvm::findClassAnyLoader(env, kCameraClass);
    jclass vec = dt::jvm::findClassAnyLoader(env, kVec3dClass);
    jclass mat = dt::jvm::findClassAnyLoader(env, kMatrix4fClass);
    if (!mc || !gr || !cam || !vec || !mat) {
        dbg::log("capture install: class resolve fail mc=%p gr=%p cam=%p vec=%p mat=%p",
                 (void*)mc, (void*)gr, (void*)cam, (void*)vec, (void*)mat);
        return false;
    }

    g_mc_getInstance   = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    g_mc_gameRenderer  = env->GetFieldID(mc, "field_1773", "Lnet/minecraft/class_757;");
    g_gr_getCamera     = env->GetMethodID(gr, "method_19418", "()Lnet/minecraft/class_4184;");
    g_gr_getFov        = env->GetMethodID(gr, "method_3196", "(Lnet/minecraft/class_4184;FZ)F");
    g_gr_getBasicProjectionMatrix = env->GetMethodID(gr, "method_22973", "(F)Lorg/joml/Matrix4f;");
    // The world render uses this one (worldProjectionMatrix); prefer it.
    g_gr_getProjectionMatrix = env->GetMethodID(gr, "method_74751", "(F)Lorg/joml/Matrix4f;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    // Exact view rotation (includes roll / hurt-tilt that pitch+yaw alone miss).
    g_cam_getRotation = env->GetMethodID(cam, "method_23767", "()Lorg/joml/Quaternionf;");
    if (jclass q = env->FindClass("org/joml/Quaternionf")) {
        g_q_x = env->GetFieldID(q, "x", "F");
        g_q_y = env->GetFieldID(q, "y", "F");
        g_q_z = env->GetFieldID(q, "z", "F");
        g_q_w = env->GetFieldID(q, "w", "F");
        env->DeleteLocalRef(q);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_cam_pos   = env->GetFieldID(cam, "field_18712", "Lnet/minecraft/class_243;");
    g_cam_yaw   = env->GetFieldID(cam, "field_18718", "F");
    g_cam_pitch = env->GetFieldID(cam, "field_18717", "F");
    g_vx = env->GetFieldID(vec, "field_1352", "D");
    g_vy = env->GetFieldID(vec, "field_1351", "D");
    g_vz = env->GetFieldID(vec, "field_1350", "D");

    // Optional: real render tick-progress for the fov lerp.
    g_mc_getRenderTickCounter = env->GetMethodID(mc, "method_61966", "()Lnet/minecraft/class_9779;");
    if (jclass rtc = dt::jvm::findClassAnyLoader(env, kRenderTickCounter)) {
        g_rtc_getTickProgress = env->GetMethodID(rtc, "method_60637", "(Z)F");
        env->DeleteLocalRef(rtc);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    bool mat_ok = true;
    char nm[4] = { 'm', 0, 0, 0 };
    for (int col = 0; col < 4 && mat_ok; ++col)
        for (int row = 0; row < 4; ++row) {
            nm[1] = (char)('0' + col); nm[2] = (char)('0' + row);
            jfieldID f = env->GetFieldID(mat, nm, "F");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!f) { mat_ok = false; break; }
            g_mat[col*4+row] = f;
        }

    g_mc_class = static_cast<jclass>(env->NewGlobalRef(mc));
    env->DeleteLocalRef(mc); env->DeleteLocalRef(gr); env->DeleteLocalRef(cam);
    env->DeleteLocalRef(vec); env->DeleteLocalRef(mat);

    const bool ok = g_mc_getInstance && g_mc_gameRenderer && g_gr_getCamera
        && g_gr_getFov && g_gr_getBasicProjectionMatrix
        && g_cam_pos && g_cam_yaw && g_cam_pitch && g_vx && g_vy && g_vz
        && mat_ok && g_mc_class;
    g_ready.store(ok, std::memory_order_release);
    dbg::log("capture install: reflection %s [getInstance=%p gameRenderer=%p getCamera=%p "
             "getFov=%p getProj=%p cam_pos=%p cam_yaw=%p mat_ok=%d]",
             ok ? "OK (no-hook JNI read)" : "INCOMPLETE",
             (void*)g_mc_getInstance, (void*)g_mc_gameRenderer, (void*)g_gr_getCamera,
             (void*)g_gr_getFov, (void*)g_gr_getBasicProjectionMatrix,
             (void*)g_cam_pos, (void*)g_cam_yaw, (int)mat_ok);
    return ok;
}

void capture_now(JNIEnv* env, bool force)
{
    if (!force && !g_on.load(std::memory_order_acquire)) return;
    if (!g_ready.load(std::memory_order_acquire)) return;
    if (!env) return;

    jobject mc = env->CallStaticObjectMethod(g_mc_class, g_mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;

    jobject grr = env->GetObjectField(mc, g_mc_gameRenderer);
    if (!grr) { env->DeleteLocalRef(mc); return; }

    jobject camera = env->CallObjectMethod(grr, g_gr_getCamera);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!camera) { env->DeleteLocalRef(grr); env->DeleteLocalRef(mc); return; }

    render_frame f{};

    // Real render tick-progress so the fov lerp matches the rendered frame
    // (passing 1.0 lags the projection a tick behind during zoom animations).
    float td = 1.0f;
    if (g_mc_getRenderTickCounter && g_rtc_getTickProgress) {
        jobject rtc = env->CallObjectMethod(mc, g_mc_getRenderTickCounter);
        if (rtc) { td = env->CallFloatMethod(rtc, g_rtc_getTickProgress, JNI_TRUE); env->DeleteLocalRef(rtc); }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Projection: MC's exact matrix for the current fov. Prefer the WORLD
    // projection (what renderWorld uses) over the basic one.
    const float fov = env->CallFloatMethod(grr, g_gr_getFov, camera, td, JNI_TRUE);
    // NOTE: getBasicProjectionMatrix(fov) is the one that takes an FOV. The other
    // overload (method_74751) takes a different float and produced a wrong matrix
    // (tiny wireframe while zoomed), so it is deliberately not used here.
    jobject proj = env->CallObjectMethod(grr, g_gr_getBasicProjectionMatrix, fov);
    if (env->ExceptionCheck()) env->ExceptionClear();
    read_matrix(env, proj, f.projection);
    if (proj) env->DeleteLocalRef(proj);

    // Modelview: pitch/yaw only, deliberately. Camera.rotation is built from the
    // same two angles (Camera.setRotation does rotationXYZ on them and nothing
    // else), so it carries no roll of its own and the two paths agree.
    //
    // The damage tilt and the walking bob are NOT here and never were: the game
    // applies both to the matrix stack inside renderWorld, after the camera
    // rotation is chosen. They are supplied by the bob pass in esp.cpp and
    // outline_view.cpp, which now calls tiltViewWhenHurt as well as bobView --
    // this comment used to claim that pass already handled the tilt while that
    // pass claimed THIS one did, so between them nobody applied it.
    const float pitch = env->GetFloatField(camera, g_cam_pitch);
    const float yaw   = env->GetFloatField(camera, g_cam_yaw);
    build_modelview(pitch, yaw, f.modelview);

    // Camera world position.
    jobject vec = env->GetObjectField(camera, g_cam_pos);
    if (vec) {
        f.cam_x = env->GetDoubleField(vec, g_vx);
        f.cam_y = env->GetDoubleField(vec, g_vy);
        f.cam_z = env->GetDoubleField(vec, g_vz);
        env->DeleteLocalRef(vec);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    int w = 0, h = 0;
    core::gl_hook::framebuffer_size(w, h);
    f.viewport_w = w; f.viewport_h = h;
    f.valid = (w > 0 && h > 0);
    const unsigned long long n = g_fires.fetch_add(1, std::memory_order_relaxed) + 1;
    f.frame = n;
    if (f.valid) publish_frame(f);

    dbg::log_rl(1, 2000,
        "capture: read frame vw=%d vh=%d fov=%.1f cam=(%.1f,%.1f,%.1f) pitch=%.1f yaw=%.1f "
        "proj[0,5,10,14]=%.3f,%.3f,%.3f,%.3f mv[0,5,10]=%.3f,%.3f,%.3f",
        f.viewport_w, f.viewport_h, fov, f.cam_x, f.cam_y, f.cam_z, pitch, yaw,
        f.projection[0], f.projection[5], f.projection[10], f.projection[14],
        f.modelview[0], f.modelview[5], f.modelview[10]);

    env->DeleteLocalRef(camera);
    env->DeleteLocalRef(grr);
    env->DeleteLocalRef(mc);
}

void shutdown() { g_on.store(false, std::memory_order_release); }
bool is_installed() { return g_ready.load(std::memory_order_acquire); }
void set_capture_enabled(bool on) { g_on.store(on, std::memory_order_release); }
unsigned long long debug_fire_count() { return g_fires.load(std::memory_order_relaxed); }

} // namespace dt::modules::hitfx::capture
