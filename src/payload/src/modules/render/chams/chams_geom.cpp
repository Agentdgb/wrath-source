#include "chams_geom.h"

#include <cmath>

namespace vanta::modules::chams::geom {

namespace {

// Near-plane clip distance, in clip-space w.
//
// For Minecraft's perspective projection w is the view-space distance, and MC's
// own near plane is 0.05 blocks. Clipping at the same distance means the module
// keeps exactly the geometry the game keeps, and — the part that matters — it
// bounds what the perspective divide can produce.
//
// The first version used 1e-4 here. That is not a near plane, it is a
// divide-by-zero guard, and using it as a clip plane let a vertex project to
// ~1.3e7 pixels on a 2560-wide viewport. At 0.05 the same vertex lands within
// a small multiple of the screen, which is what the rasteriser can afford.
constexpr float kNearW = 0.05f;

// Below this the polygon is a sliver or a degenerate fan; drawing it costs a
// draw call and shows nothing.
constexpr float kMinArea = 0.05f;

} // namespace

const int kFaces[6][4] = {
    { 0, 1, 3, 2 },   // -X
    { 4, 6, 7, 5 },   // +X
    { 0, 4, 5, 1 },   // -Y  (underside)
    { 2, 3, 7, 6 },   // +Y  (top)
    { 0, 2, 6, 4 },   // -Z
    { 1, 5, 7, 3 },   // +Z
};

const int kEdges[12][2] = {
    {0,4},{1,5},{2,6},{3,7},
    {0,2},{1,3},{4,6},{5,7},
    {0,1},{2,3},{4,5},{6,7},
};

const float kFaceShade[6] = {
    0.78f,   // -X side
    0.78f,   // +X side
    0.55f,   // -Y underside, darkest
    1.00f,   // +Y top, brightest
    0.66f,   // -Z
    0.66f,   // +Z
};

// ─────────────────────────────────────────────────────────────────────────────
clip_vertex to_clip(const camera& c, const rel_point& p)
{
    const float* m = c.mvp;
    clip_vertex v;
    v.x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
    v.y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
    v.z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    v.w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    return v;
}

void to_screen(const camera& c, const clip_vertex& v, float& sx, float& sy)
{
    const float inv = 1.f / v.w;
    sx = (v.x * inv * 0.5f + 0.5f) * c.vw;
    sy = c.vh - (v.y * inv * 0.5f + 0.5f) * c.vh;
}

// ─────────────────────────────────────────────────────────────────────────────
bool face_faces_camera(const rel_point corners[8], const int face[4])
{
    const rel_point& a = corners[face[0]];
    const rel_point& b = corners[face[1]];
    const rel_point& d = corners[face[3]];

    // Two edges from the first corner.
    const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
    const float e2x = d.x - a.x, e2y = d.y - a.y, e2z = d.z - a.z;

    // Outward normal (the winding in kFaces is consistent across all six, so
    // the cross product points the same way relative to the box everywhere;
    // whether that is "out" or "in" is settled by the dot below being taken
    // against the face centre, which is unambiguous).
    const float nx = e1y * e2z - e1z * e2y;
    const float ny = e1z * e2x - e1x * e2z;
    const float nz = e1x * e2y - e1y * e2x;

    // Face centre, camera-relative. The camera is the origin in this space, so
    // the vector from the centre to the camera is just -centre.
    const float cx = (corners[face[0]].x + corners[face[1]].x +
                      corners[face[2]].x + corners[face[3]].x) * 0.25f;
    const float cy = (corners[face[0]].y + corners[face[1]].y +
                      corners[face[2]].y + corners[face[3]].y) * 0.25f;
    const float cz = (corners[face[0]].z + corners[face[1]].z +
                      corners[face[2]].z + corners[face[3]].z) * 0.25f;

    const float dot = nx * (-cx) + ny * (-cy) + nz * (-cz);

    // |dot| below this is the camera lying in the face plane — the face is
    // edge-on and contributes no pixels either way.
    return std::fabs(dot) > 1e-6f && dot > 0.f;
}

// ─────────────────────────────────────────────────────────────────────────────
int clip_and_project_face(const camera& c, const clip_vertex corners[8],
                          const int face[4], float out_x[5], float out_y[5])
{
    // Sutherland-Hodgman against the single plane w = kNearW, in clip space.
    // Linear interpolation is exact here because the perspective divide has not
    // happened yet — which is the whole reason the clip lives on this side of
    // it rather than on screen coordinates.
    clip_vertex poly[5];
    int n = 0;

    for (int i = 0; i < 4; ++i) {
        const clip_vertex& cur = corners[face[i]];
        const clip_vertex& nxt = corners[face[(i + 1) & 3]];
        const bool cur_in = cur.w >= kNearW;
        const bool nxt_in = nxt.w >= kNearW;

        if (cur_in && n < 5) poly[n++] = cur;

        if (cur_in != nxt_in && n < 5) {
            const float denom = nxt.w - cur.w;
            if (std::fabs(denom) > 1e-9f) {
                const float t = (kNearW - cur.w) / denom;
                clip_vertex v;
                v.x = cur.x + (nxt.x - cur.x) * t;
                v.y = cur.y + (nxt.y - cur.y) * t;
                v.z = cur.z + (nxt.z - cur.z) * t;
                v.w = kNearW;
                poly[n++] = v;
            }
        }
    }

    if (n < 3) return 0;

    for (int i = 0; i < n; ++i)
        to_screen(c, poly[i], out_x[i], out_y[i]);

    // A sliver contributes nothing but costs a draw entry.
    if (std::fabs(signed_area(out_x, out_y, n)) < kMinArea) return 0;

    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
bool offscreen(const float* xs, const float* ys, int n, float vw, float vh)
{
    if (n < 3) return true;
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; ++i) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    return maxx < 0.f || minx > vw || maxy < 0.f || miny > vh;
}

float signed_area(const float* xs, const float* ys, int n)
{
    if (n < 3) return 0.f;
    float a = 0.f;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        a += xs[i] * ys[j] - xs[j] * ys[i];
    }
    return a * 0.5f;
}

} // namespace vanta::modules::chams::geom
