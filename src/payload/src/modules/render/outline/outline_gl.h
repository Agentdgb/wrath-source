// outline_gl.h — the three-pass silhouette pipeline. See outline.h for what the
// passes are and why they are shaped this way.
//
// Owns its framebuffers and its shaders and nothing else: it is handed a flat
// list of triangles already in framebuffer pixels and a colour, and it paints
// the contour of their union onto whatever framebuffer the caller names. It
// knows nothing about players, items, poses or Minecraft.

#pragma once

#include "modules/render/arraylist/al_gl.h"

namespace vanta::modules::outline::gl {

struct params {
    float thickness_px = 2.f;    // width of the solid line, framebuffer pixels
    float glow_px      = 0.f;    // halo reach; 0 disables the halo entirely
    // How much light the halo adds where it starts, i.e. at the LINE's outer
    // edge. Screened onto the destination rather than painted over it, so it is
    // bounded by construction: 1.0 cannot blow a bright background out to
    // white, it only stops adding.
    float glow_alpha   = 0.85f;
    float col_r = 1.f, col_g = 0.2f, col_b = 0.3f, col_a = 1.f;
};

// A rectangle of the framebuffer, in pixels, bottom-left origin. Every pass is
// clipped to this, so a lone player in a corner costs a corner's worth of work
// rather than a screen's.
struct region {
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
};


class pipeline {
public:
    pipeline() = default;
    ~pipeline() { destroy(); }
    pipeline(const pipeline&)            = delete;
    pipeline& operator=(const pipeline&) = delete;

    // `xy` is two floats per vertex, three vertices per triangle, in
    // framebuffer pixels with a BOTTOM-LEFT origin (gl_FragCoord's convention,
    // not ImGui's). Silently does nothing when the loader or the shaders failed.
    void render(const float* xy, int vertex_count,
                const region& box,
                int display_w, int display_h,
                unsigned int composite_fbo,
                const params& p);

    void destroy();

private:
    void ensure_shaders();
    bool ensure_mesh();
    void upload(const float* xy, int vertex_count);

    ::vanta::al::al_shader fill_{};      // pass 1: flat white triangles
    ::vanta::al::al_shader dist_h_{};    // pass 2a: nearest lit texel on the row
    ::vanta::al::al_shader dist_v_{};    // pass 2b: combine rows -> true distance
    ::vanta::al::al_shader draw_{};      // pass 3: threshold into line + halo

    bool shaders_tried_ = false;
    bool shaders_ok_    = false;

    ::vanta::al::al_quad quad_{};        // the region quad the last three passes use
    ::vanta::al::al_fbo  mask_{};
    ::vanta::al::al_fbo  tmp_{};         // ping for the distance transform
    ::vanta::al::al_fbo  dist_{};        // pong, and what pass 3 reads

    unsigned int vao_ = 0;               // the triangle soup for pass 1
    unsigned int vbo_ = 0;
};

} // namespace vanta::modules::outline::gl
