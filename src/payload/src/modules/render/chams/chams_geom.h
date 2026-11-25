// chams_geom.h — projection, near-plane clipping and face topology.
//
// Pure arithmetic, like chams_pose: no JNI, no GL, no ImGui. Testable offline.
//
// Two things here were got WRONG in the first shipping version, and both are
// worth stating plainly because the corrected design is shaped by them.
//
// 1) The near plane was clipped at w > 1e-4. A vertex sitting on that plane
//    divides by 1e-4, so with a 2560-wide viewport its screen coordinate comes
//    out around +/-1.3e7 pixels. Hundreds of those per frame, alpha-blended,
//    is a fill-rate wall: the measured framerate fell from ~275 to ~30. The
//    clip now happens at a real near distance (kNearW below), which bounds
//    screen coordinates to a sane multiple of the viewport.
//
// 2) Backface culling inferred a winding sign from the LARGEST projected face
//    area. That is only sound while nothing is clipped — a near-plane-clipped
//    back face can out-area every front face, flip the inferred sign, and cull
//    exactly the faces that were visible, so the body vanishes. It vanished
//    precisely when the camera moved, because that is when boxes cross the near
//    plane. Culling is now done in WORLD space from the face normal, before any
//    projection, so clipping cannot influence it at all.
//
// The lesson encoded here: never derive a geometric decision from post-
// projection quantities when the pre-projection ones are available.

#pragma once

namespace dt::modules::chams::geom {

// The captured camera for one frame. mvp is column-major (projection *
// modelview, org.joml / GL layout), matching hitfx's render_frame.
struct camera {
    float  mvp[16] = {};
    double cam_x = 0.0, cam_y = 0.0, cam_z = 0.0;
    float  vw = 0.f, vh = 0.f;
    bool   valid = false;
};

// A point after the model-view-projection multiply, before the divide.
struct clip_vertex {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

// Camera-relative world offset. Kept as float: these are small numbers (a
// player is metres away, not thousands), and keeping them separate from the
// absolute world position is what avoids the precision loss of subtracting two
// large doubles inside the inner loop.
struct rel_point {
    float x = 0.f, y = 0.f, z = 0.f;
};

// Camera-relative point -> clip space.
clip_vertex to_clip(const camera& c, const rel_point& p);

// Clip space -> framebuffer pixels. Only valid when v.w >= kNearW.
void to_screen(const camera& c, const clip_vertex& v, float& sx, float& sy);

// ─────────────────────────────────────────────────────────────────────────────
// Box topology. Corner index is (xi*4 + yi*2 + zi), which is the order
// pose::part_corners emits:
//
//     0 (lo,lo,lo)  1 (lo,lo,hi)  2 (lo,hi,lo)  3 (lo,hi,hi)
//     4 (hi,lo,lo)  5 (hi,lo,hi)  6 (hi,hi,lo)  7 (hi,hi,hi)
extern const int kFaces[6][4];
extern const int kEdges[12][2];

// Per-face brightness for the optional flat shading, indexed like kFaces.
extern const float kFaceShade[6];

// ─────────────────────────────────────────────────────────────────────────────
// Backface test, in WORLD space, before any projection.
//
// Builds the face normal from two edges of the quad and dots it against the
// vector from the face centre to the camera (which, in camera-relative
// coordinates, is simply -centre). Positive means the face turns toward the
// camera.
//
// Robust under clipping by construction, and it needs no per-box calibration.
// Returns false for a degenerate face (zero-area, camera in the face plane),
// which for a closed box is harmless: the opposite face carries the silhouette.
bool face_faces_camera(const rel_point corners[8], const int face[4]);

// ─────────────────────────────────────────────────────────────────────────────
// Clip one quad against the near plane and project the survivors.
//
// Writes up to 5 screen-space points (one plane can add at most one vertex) and
// returns how many. Returns 0 when the face is entirely nearer than the near
// plane.
int clip_and_project_face(const camera& c, const clip_vertex corners[8],
                          const int face[4], float out_x[5], float out_y[5]);

// Screen-space rejection: true when the polygon's bounding box misses the
// viewport entirely, so it can be dropped before it reaches the draw list.
// Cheap, and it is what keeps a clipped-but-offscreen face from costing
// anything.
bool offscreen(const float* xs, const float* ys, int n, float vw, float vh);

// Signed area of a projected polygon, in pixels squared. Retained for the
// degenerate-polygon guard and for diagnostics — NOT for culling any more.
float signed_area(const float* xs, const float* ys, int n);

} // namespace dt::modules::chams::geom
