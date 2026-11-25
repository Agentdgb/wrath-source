// outline.cpp — engine orchestration. See outline.h.

#include "outline.h"
#include "outline_gl.h"
#include "outline_view.h"

#include "modules/render/chams/chams_geom.h"
#include "modules/render/chams/chams_jni.h"
#include "modules/render/chams/chams_pose.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"

#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace hf   = dt::modules::hitfx;
namespace cap  = dt::modules::hitfx::capture;
namespace pose = dt::modules::chams::pose;
namespace geom = dt::modules::chams::geom;
namespace cjni = dt::modules::chams::jni;

namespace dt::modules::outline {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

constexpr float k_deg = 3.14159265358979323846f / 180.f;

/* How far out figures are traced. Not a setting: the line is drawn from the
   game's own projection, so a player who is on screen is a player who should
   have one, and a range dial only ever produces the bug report "it stops
   working at a distance". 128 is Minecraft's own entity render limit. */
constexpr float k_player_range = 128.f;
constexpr int   k_max_players  = 64;

/* The panel's Thickness and Glow size are multipliers, not pixels. These turn
   them into pixels: 0.50 thickness is a two-pixel line, and the default 2.5
   glow is a five-pixel halo. */
constexpr float k_thickness_px_per_unit = 4.f;
constexpr float k_glow_px_per_unit      = 2.f;

/* Sneaking. pose::build_pose reproduces the ANGLES vanilla applies while
   sneaking but not the part TRANSLATIONS that go with them, so they are applied
   here, to the corners, after the part has been rotated about its own pivot.
   That is equivalent to shifting the pivot and the box together the way the
   PlayerESP does -- rotating about a shifted pivot and then translating gives
   the same point either way -- and it keeps the shared pose library free of a
   caller's convention.

   Legs also move backward, and +z is forward in this space, so the Z offset is
   negative. Both tables are in entity-local blocks and are scaled with the
   model, exactly as the ESP scales them. */
const float k_sneak_dy[pose::kPartCount] = { -0.2625f, -0.2f, -0.2f, -0.2f, -0.0125f, -0.0125f };
const float k_sneak_dz[pose::kPartCount] = {  0.f,      0.f,   0.f,   0.f,  -0.25f,   -0.25f   };

gl::pipeline g_pipeline{};

std::vector<cjni::player_sample> g_players;

/* Two floats a vertex, three vertices a triangle, rebuilt every frame. Kept at
   namespace scope so the allocation survives; clear() does not release it. */
std::vector<float> g_verts;

float g_min_x = 0.f, g_min_y = 0.f, g_max_x = 0.f, g_max_y = 0.f;
bool  g_any   = false;

long long now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// out = a * b, both column-major 4x4.
void mat_mul(const float* a, const float* b, float* out)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
}

/* @the view bob, and why the contour swims without it.
 *
 * GameRenderer.bobView pushes an extra rock-and-sway transform for the world
 * pass only. The world is drawn through it; the captured modelview does not
 * contain it. Project from the captured matrices alone and the game draws the
 * player in one place while this module traces them in another, with the gap
 * opening and closing in time with your footsteps -- which is precisely what
 * "the outline moves when I move" is. chams.h records the same gap in its own
 * build_camera and calls it known and bounded; the PlayerESP folds the bob in
 * and has never had the symptom.
 *
 * `bob` is column-major and PRE-multiplied, matching esp.cpp's build_proj:
 * projection * (bob * modelview).
 */
bool build_camera(const hf::render_frame& f, const float* bob, geom::camera& out)
{
    if (!f.valid || f.viewport_w <= 0 || f.viewport_h <= 0) return false;

    float mv[16];
    if (bob) mat_mul(bob, f.modelview, mv);
    else     std::memcpy(mv, f.modelview, sizeof(mv));

    mat_mul(f.projection, mv, out.mvp);

    out.cam_x = f.cam_x; out.cam_y = f.cam_y; out.cam_z = f.cam_z;
    out.vw = static_cast<float>(f.viewport_w);
    out.vh = static_cast<float>(f.viewport_h);
    out.valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
/* One box, camera-relative, into triangles.
 *
 * All six faces are emitted, front and back alike. That is not an oversight and
 * it is not waste worth removing: the union of a box's six projected faces IS
 * its projected silhouette, so filling all six gets the right answer without a
 * backface test — and chams_geom.h records that the backface test was got wrong
 * twice, both times in ways that made limbs vanish exactly when the camera
 * moved. The mask does not care how many times a texel is covered.
 */
/* One vertex, and the running screen bounds with it. Every pass downstream is
   clipped to those bounds, so they have to see every point that is drawn. */
void push_vert(float x, float y)
{
    if (!g_any) {
        g_min_x = g_max_x = x;
        g_min_y = g_max_y = y;
        g_any = true;
    } else {
        g_min_x = std::min(g_min_x, x);
        g_max_x = std::max(g_max_x, x);
        g_min_y = std::min(g_min_y, y);
        g_max_y = std::max(g_max_y, y);
    }
    g_verts.push_back(x); g_verts.push_back(y);
}

void emit_box(const geom::camera& cam, const geom::rel_point rel[8],
              float sx_scale, float sy_scale, float fb_h)
{
    geom::clip_vertex cv[8];
    for (int i = 0; i < 8; ++i) cv[i] = geom::to_clip(cam, rel[i]);

    for (int f = 0; f < 6; ++f) {
        float xs[5], ys[5];
        const int n = geom::clip_and_project_face(cam, cv, geom::kFaces[f], xs, ys);
        if (n < 3) continue;
        if (geom::offscreen(xs, ys, n, cam.vw, cam.vh)) continue;

        /* chams_geom projects into VIEWPORT pixels with a top-left origin,
           which is ImGui's convention. GL's is bottom-left, and the framebuffer
           is not guaranteed to be the viewport, so both are converted here
           rather than assumed away. */
        float gx[5], gy[5];
        for (int i = 0; i < n; ++i) {
            gx[i] = xs[i] * sx_scale;
            gy[i] = fb_h - ys[i] * sy_scale;
        }

        for (int i = 1; i + 1 < n; ++i) {
            push_vert(gx[0],     gy[0]);
            push_vert(gx[i],     gy[i]);
            push_vert(gx[i + 1], gy[i + 1]);
        }
    }
}

void emit_player(const geom::camera& cam, const cjni::player_sample& p,
                 float sx_scale, float sy_scale, float fb_h)
{
    pose::part_def parts[pose::kPartCount];
    for (int i = 0; i < pose::kPartCount; ++i) parts[i] = pose::kParts[i];
    if (p.slim) pose::apply_slim_arms(parts);

    pose::part_pose poses[pose::kPartCount];
    pose::build_pose(p.pose, parts, poses);

    const float yaw = p.pose.body_yaw_deg * k_deg;
    const float ct = std::cos(yaw), st = std::sin(yaw);

    /* @the whole body tipped over, for a glide or a swim.

       Vanilla rotates the ENTIRE entity about X before the model is posed, so a
       gliding player lies flat. Nothing in a per-part pose can express that, and
       without it the contour stands the figure upright while the game draws it
       horizontal — a whole figure out of place, which is what an elytra looked
       like.

       Applied in the PlayerESP's frame, deliberately: this module's corners are
       turned 180 degrees into that frame two lines below anyway, and the ESP's
       swim chain is the one that has been on screen and is known to be right.
       Re-deriving the sign in chams' frame is how the last two rotation bugs in
       this file happened. */
    const float cl = std::cos(p.pose.lean_rad), sl = std::sin(p.pose.lean_rad);
    const float swim_dy = p.pose.swim_pose ? -1.0f  : 0.f;
    const float swim_dz = p.pose.swim_pose ? -0.3f  : 0.f;
    const bool  leaning = (p.pose.lean_rad > 0.0001f || p.pose.lean_rad < -0.0001f);

    for (int i = 0; i < pose::kPartCount; ++i) {
        /* @the BASE layer, with no dilation. This was the outer layer, and the
           head is what gave it away.

           Minecraft's hat layer is the head cuboid inflated by half a model
           unit on every side -- a 12.5% wider head -- and the jacket and
           sleeves add half that again to the body and arms. Tracing the outer
           layer therefore stands the line a fixed FRACTION of the figure away
           from it, which at four metres is about ten pixels clear of the skull
           on each side. That is the slack that was reported, and it is why it
           showed on the head first: the head's dilation is twice everything
           else's.

           The trade is stated rather than hidden: a skin whose hat layer really
           is opaque -- a hood, big hair -- now has that shell fall just outside
           the line. That is a thin band on some skins, against a permanent gap
           on every skin, and the permanent gap is the one that reads as
           broken. */
        float local[8][3];
        pose::part_corners(parts[i], poses[i], /*dilation=*/0.f,
                           p.pose.scale, local);

        const float sdy = p.pose.sneaking ? k_sneak_dy[i] * p.pose.scale : 0.f;
        const float sdz = p.pose.sneaking ? k_sneak_dz[i] * p.pose.scale : 0.f;

        geom::rel_point rel[8];
        for (int c = 0; c < 8; ++c) {
            /* @turn the part round before anything else touches it.
             *
             * chams::pose and the PlayerESP describe the same pose in frames
             * that are 180 degrees apart about Y. Both are self-consistent:
             * put an item in a player's hand and chams sends it to -z while
             * the ESP sends it to +z, and each is "forward" in its own frame.
             *
             * This module took chams' corners and applied the ESP's body-yaw
             * formula to them, which left every figure facing backwards. At
             * rest a player is nearly symmetric front to back, so it looked
             * right; the moment a head pitched or an arm swung, the contour
             * moved the OPPOSITE way from the model and tore out of the
             * silhouette. That is the "it bugs out when he looks down" and the
             * "it bugs out when he hits" in one cause.
             *
             * Measured, not argued: with this flip the two independently
             * written pose stacks agree to 0.0009 blocks at every body yaw —
             * which is the 1.501-versus-1.50 rounding chams_pose.h documents.
             * Without it they are 0.98 blocks apart.
             *
             * Done here rather than in chams::pose because that library is
             * right; it is this caller that mixed two conventions. The sneak
             * offsets are applied AFTER the flip, in the frame their table was
             * written for. */
            const float lx = -local[c][0];
            float       ly =  local[c][1] + sdy;
            float       lz = -local[c][2] + sdz;

            /* The root tip, about the feet, after the swim pose's own shift.
               Both are in the frame the flip above just put us in. */
            if (leaning) {
                const float ty = ly + swim_dy;
                const float tz = lz + swim_dz;
                ly = ty * cl - tz * sl;
                lz = ty * sl + tz * cl;
            }

            const double wx = p.render_x + static_cast<double>(lx * ct - lz * st);
            const double wy = p.render_y + static_cast<double>(ly);
            const double wz = p.render_z + static_cast<double>(lx * st + lz * ct);

            rel[c].x = static_cast<float>(wx - cam.cam_x);
            rel[c].y = static_cast<float>(wy - cam.cam_y);
            rel[c].z = static_cast<float>(wz - cam.cam_z);
        }

        emit_box(cam, rel, sx_scale, sy_scale, fb_h);
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
void install()
{
    // Shared with hitfx, the ESP and the chams; installing is idempotent, so
    // whichever module gets there first pays for it.
    cap::install();
    VLOG_INFO("outline", "install() done");
}

void shutdown()
{
    g_players.clear();
    g_verts.clear();

    ::dt::jvm::JniScope s;
    if (s) view::release_cache(s.get());
}

// ─────────────────────────────────────────────────────────────────────────────
void render_background(int display_w, int display_h, unsigned int composite_fbo)
{
    if (g_settings_provider) g_settings_provider(g_settings);

    if (!g_enabled || display_w < 4 || display_h < 4) {
        if (!g_players.empty()) g_players.clear();
        return;
    }

    /* Deliberately NOT hidden behind the ClickGui. This pass runs before ImGui
       exists, so the panel is composited over it either way — and leaving it
       running is what lets the Thickness and Glow sliders be watched against a
       real figure while they are being dragged. */

    geom::camera cam{};

    /* Cadence. The players are sampled EVERY frame: unlike the chams, whose
       effect is Minecraft's own draw replayed, this module's geometry is what
       gets drawn, so sampling it at anything below the framerate is visible
       directly as the line stuttering behind the figure. The PlayerESP pays the
       same price for the same reason. */
    const long long now = now_ns();

    {
        ::dt::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();

        /* Retry the install until a world exists, then arm the service before
           asking it for a frame. capture_now() opens with an enabled check, so
           calling it without set_capture_enabled is a silent no-op that leaves
           latest_frame() handing back valid=0 forever. */
        if (!cap::is_installed()) {
            static long long s_next_try_ns = 0;
            if (now >= s_next_try_ns) { s_next_try_ns = now + 1'000'000'000LL; cap::install(); }
            return;
        }

        cap::set_capture_enabled(true);
        cap::capture_now(env);

        /* MC's own view bob, read back out of GameRenderer rather than
           reimplemented. Optional: without it the contour still tracks a
           standing player exactly and only drifts while walking. */
        float bob[16];
        const bool have_bob = view::ensure_cache(env) && view::bob_matrix(env, bob);

        hf::render_frame f{};
        if (!hf::latest_frame(f) || !build_camera(f, have_bob ? bob : nullptr, cam)) return;

        if (!cjni::ensure_cache(env)) return;

        /* No skin and no line of sight. Both are several JNI calls a player,
           and this poll runs at the framerate — the line is drawn from the pose
           and the position, and it is drawn whether or not a wall is in the
           way, so neither answer would ever be read. */
        cjni::poll_players(env, cam.cam_x, cam.cam_y, cam.cam_z,
                           k_player_range, /*include_self=*/false,
                           k_max_players, /*want_visibility=*/false,
                           /*want_skin=*/false, g_players);

    }

    // ── Geometry ────────────────────────────────────────────────────────────
    g_verts.clear();
    g_any = false;

    const float fb_w = static_cast<float>(display_w);
    const float fb_h = static_cast<float>(display_h);
    const float sx_scale = (cam.vw > 1.f) ? fb_w / cam.vw : 1.f;
    const float sy_scale = (cam.vh > 1.f) ? fb_h / cam.vh : 1.f;

    for (const cjni::player_sample& p : g_players)
        emit_player(cam, p, sx_scale, sy_scale, fb_h);

    if (!g_any || g_verts.size() < 6) return;

    // ── Draw ────────────────────────────────────────────────────────────────
    gl::params p{};
    p.thickness_px = std::max(0.6f, g_settings.thickness * k_thickness_px_per_unit);
    p.glow_px      = g_settings.glow ? g_settings.glow_size * k_glow_px_per_unit : 0.f;
    p.glow_alpha   = g_settings.glow_alpha;
    p.col_r = g_settings.col_r;
    p.col_g = g_settings.col_g;
    p.col_b = g_settings.col_b;
    p.col_a = g_settings.col_a;

    gl::region box{};
    box.min_x = g_min_x; box.min_y = g_min_y;
    box.max_x = g_max_x; box.max_y = g_max_y;

    g_pipeline.render(g_verts.data(), static_cast<int>(g_verts.size() / 2),
                      box, display_w, display_h, composite_fbo, p);
}

} // namespace dt::modules::outline
