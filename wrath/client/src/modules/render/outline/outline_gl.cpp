// outline_gl.cpp — the three passes. See outline_gl.h and outline.h.

#include "outline_gl.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif

namespace vanta::modules::outline::gl {

namespace {

/* @the buffers pick their own resolution, and the rule is affordability.

   Thresholding an INTERPOLATED DISTANCE keeps the LINE sharp at any buffer
   size — that is how signed-distance text works — so half resolution never
   softens the contour itself. What it costs is the ability to separate two
   features closer together than two pixels, which only matters for a figure
   smaller than the line is wide; this module traces players, and a player is
   never that small.

   So the buffers run at FULL resolution whenever the search radius stays
   cheap, and drop to half only when a wide contour would otherwise make the
   two distance passes expensive. Both passes are O(R) per texel over four
   times the texels, which is what this threshold rations — and R is driven by
   the LINE PLUS THE HALO, not by whichever is larger, so a thick line reaches
   the switch sooner than the glow alone would suggest. */
constexpr float kFullResReach = 24.f;

/* Search radius cap, in half-resolution texels. Both distance passes are O(R),
   so this is what bounds their cost; 24 half-texels is 48 framebuffer pixels of
   reach, more than the widest glow the panel offers. Kept in step with the loop
   bound written into the two shaders below. */
constexpr float kMaxRadius = 24.f;

const char* k_fill_vsh = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uInvSize;
void main() {
    gl_Position = vec4(aPos * uInvSize * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* k_fill_fsh = R"GLSL(
#version 330 core
out vec4 frag;
void main() { frag = vec4(1.0); }
)GLSL";

/* The last three passes all draw the same rectangle of the screen, so they
   share a vertex shader that maps the unit quad onto it. */
const char* k_region_vsh = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uMin;
uniform vec2 uMax;
out vec2 vUv;
void main() {
    vec2 t = aPos * 0.5 + 0.5;
    vUv = mix(uMin, uMax, t);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

/* Pass 2a. For each texel, how far along its own row the nearest lit texel is.
   Scanning outward and stopping at the first hit is what makes it a minimum. */
const char* k_dist_h_fsh = R"GLSL(
#version 330 core
uniform sampler2D uMask;
uniform vec2  uTexel;
uniform float uR;
in  vec2 vUv;
out vec4 frag;
void main() {
    float far  = uR + 1.0;
    float best = far;
    if (texture(uMask, vUv).a > 0.5) {
        best = 0.0;
    } else {
        for (int i = 1; i <= 24; ++i) {
            float fi = float(i);
            if (fi > uR) break;
            float l = texture(uMask, vec2(vUv.x - fi * uTexel.x, vUv.y)).a;
            float r = texture(uMask, vec2(vUv.x + fi * uTexel.x, vUv.y)).a;
            if (max(l, r) > 0.5) { best = fi; break; }
        }
    }
    frag = vec4(best / far, 0.0, 0.0, 1.0);
}
)GLSL";

/* Pass 2b. Combine each row's answer with how far away that row is. Rows are
   visited nearest-first, so once the best distance found is no further than the
   current row offset, no later row can beat it — which is the early exit, and
   the reason a wide glow does not cost the full radius everywhere. */
const char* k_dist_v_fsh = R"GLSL(
#version 330 core
uniform sampler2D uSrc;
uniform vec2  uTexel;
uniform float uR;
in  vec2 vUv;
out vec4 frag;
void main() {
    float far  = uR + 1.0;
    float best = far;
    for (int i = 0; i <= 24; ++i) {
        float fi = float(i);
        if (fi > uR) break;
        if (fi >= best) break;
        float hu = texture(uSrc, vec2(vUv.x, vUv.y + fi * uTexel.y)).r * far;
        float hd = texture(uSrc, vec2(vUv.x, vUv.y - fi * uTexel.y)).r * far;
        best = min(best, min(sqrt(hu * hu + fi * fi), sqrt(hd * hd + fi * fi)));
    }
    frag = vec4(min(best, far) / far, 0.0, 0.0, 1.0);
}
)GLSL";

/* Pass 3, drawn TWICE from one program: the halo screened onto the world, then
   the line composited over it. Both lobes are thresholds on the one field, so
   they can never disagree about where the edge is; what differs is only how
   each is allowed to touch what is already there.

     uPass 0 — halo.  glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE)
                      dst' = dst + src * (1 - dst)            [screen]
     uPass 1 — line.  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
                      dst' = src + dst * (1 - a)              [premultiplied over]

   @why the halo cannot share the line's blend.

   Straight alpha is a LERP toward the colour, so it can only move what is
   behind it TOWARD that colour — against anything brighter than the outline it
   SUBTRACTS light. Measured: at the old halo's real peak of 0.128, over snow it
   took Rec.709 luminance from 0.961 down to 0.885. A glow that darkens is not a
   glow. Screening adds, and the (1 - dst) term makes it self-limiting, so a
   bright background saturates instead of clipping and losing its hue the way
   pure addition does. */
const char* k_draw_fsh = R"GLSL(
#version 330 core
uniform sampler2D uDist;
uniform float uFarPx;
uniform float uBiasPx;
uniform float uThick;
uniform float uGlow;
uniform float uGlowAlpha;
uniform vec4  uColor;
uniform int   uPass;      // 0 = halo, screened; 1 = line, over
in  vec2 vUv;
out vec4 frag;
void main() {
    /* The field is measured between texel CENTRES, so the first texel outside
       the figure already reports a whole texel of distance even though it is
       sitting on the edge. Removing half a texel is what puts the line against
       the figure instead of a gap away from it. */
    float d = max(0.0, texture(uDist, vUv).r * uFarPx - uBiasPx);

    /* Nothing is ever painted over the figure itself.

       Ramped over one framebuffer PIXEL, and it has to stay in pixels: d is
       already a true pixel distance at either buffer resolution, so rescaling
       this by the texel size would make it the one resolution-dependent term in
       the shader and open a visible step at the full/half switch where there is
       none today. */
    float outside = smoothstep(0.0, 1.0, d);

    /* Analytic coverage, and it also pays back a half pixel the line has been
       losing since it shipped. clamp(T + 1 - d, 0, 1) is the exact area of a
       one-pixel box on the lit side of a band edge at d = T; the +1 rather than
       +0.5 compensates `outside`, which integrates to half a pixel less than a
       step. Asked for 2.0 px the old expression laid down 1.5, and 0.8 gave
       0.355 — every line in the panel's range was thinner than its number. */
    float line = outside * clamp(uThick + 1.0 - d, 0.0, 1.0);

    if (uPass != 0) {
        float a = line * uColor.a;

        /* One 8-bit level at the worst case. Below this the draw provably
           cannot move a byte, so it is fill spent for nothing. */
        if (a <= 0.004) discard;
        frag = vec4(uColor.rgb * a, a);
        return;
    }

    if (uGlow <= 0.5) discard;

    /* @the halo is measured from the LINE's outer edge, not from the figure.

       Measured from the figure it sat UNDER the line, and how much survived
       depended on Thickness rather than on Glow size: at an eight-pixel line
       with an eight-pixel glow the entire halo was inside the line, and the
       0.121 that came through was the line's own antialiasing fringe. That is
       why the bottom half of Glow size did nothing at high Thickness. Rebased
       here it starts where the line's nominal width ends, at every combination.

       CUBED, not squared. The look being copied is a blurred silhouette, whose
       profile outside the edge is a complementary error function; against that
       reference the worst deviation across the band is 0.039 for t^3 against
       0.130 for the shipping t^2. Squared holds a quarter of its peak at the
       halfway point where the reference holds an eighth — nearly twice the
       light, spread flat through the middle, which is what reads as an even
       wash rather than as a light. Cubed lands on zero with zero slope, so
       there is no rim at the outer edge either.

       min() keeps the screen blend's boundedness true for any strength setting;
       (1 - line) keeps the outline one decal rather than two stacked, which
       matters because the colour carries a live alpha bar and a translucent
       line would otherwise come out brighter at its core than its own setting
       asks for. */
    float t = clamp(1.0 - max(0.0, d - uThick) / uGlow, 0.0, 1.0);
    float e = min(1.0, t * t * t * uGlowAlpha * uColor.a * outside * (1.0 - line));

    if (e <= 0.004) discard;
    frag = vec4(uColor.rgb * e, e);
}
)GLSL";

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
void pipeline::ensure_shaders()
{
    if (shaders_tried_) return;
    shaders_tried_ = true;

    if (!::vanta::al::gl::api().ok) return;

    shaders_ok_ =
        fill_  .load(k_fill_vsh,   k_fill_fsh,   "outline.fill")   &&
        dist_h_.load(k_region_vsh, k_dist_h_fsh, "outline.dist_h") &&
        dist_v_.load(k_region_vsh, k_dist_v_fsh, "outline.dist_v") &&
        draw_  .load(k_region_vsh, k_draw_fsh,   "outline.draw");
}

bool pipeline::ensure_mesh()
{
    if (vao_) return true;

    const auto& g = ::vanta::al::gl::api();
    if (!g.ok) return false;

    g.GenVertexArrays(1, &vao_);
    if (!vao_) return false;

    g.BindVertexArray(vao_);
    g.GenBuffers(1, &vbo_);
    if (!vbo_) {
        g.DeleteVertexArrays(1, &vao_);
        vao_ = 0;
        return false;
    }
    g.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    g.BindVertexArray(0);
    g.BindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void pipeline::upload(const float* xy, int vertex_count)
{
    const auto& g = ::vanta::al::gl::api();

    /* One glBufferData a frame, data and all. Re-specifying the whole store is
       what tells the driver the old contents are dead, so it hands back a fresh
       allocation instead of waiting for the GPU to finish reading the last
       one. The shared loader carries no glBufferSubData, and adding an entry
       point to fill a sub-range of a buffer that is entirely rewritten every
       frame would buy nothing. */
    g.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    g.BufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertex_count * 2 * sizeof(float)),
                 xy, GL_DYNAMIC_DRAW);
    g.BindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
void pipeline::render(const float* xy, int vertex_count,
                      const region& box,
                      int display_w, int display_h,
                      unsigned int composite_fbo,
                      const params& p)
{
    ensure_shaders();
    if (!shaders_ok_ || vertex_count < 3 || display_w < 4 || display_h < 4) return;
    if (!quad_.ensure() || !ensure_mesh()) return;

    /* How far the passes have to look, in framebuffer pixels.

       The SUM, not the larger of the two. The halo is measured from the LINE's
       outer edge now, so the outermost lit texel sits at thickness + glow; a
       max() here cuts the field short and the tail stops dead against the clip
       rectangle instead of fading, which is a hard rectangular band around
       every figure. Two over the sum, so the last band is still measured.

       A function of the SETTINGS, so it only changes when a slider moves — the
       buffers below are not resized frame to frame. */
    const float reach_px = p.thickness_px + p.glow_px + 2.f;
    const int   ds = (reach_px <= kFullResReach) ? 1 : 2;
    const float dsf = static_cast<float>(ds);

    const int buf_w = std::max(2, display_w / ds);
    const int buf_h = std::max(2, display_h / ds);
    if (!mask_.ensure(buf_w, buf_h) ||
        !tmp_ .ensure(buf_w, buf_h) ||
        !dist_.ensure(buf_w, buf_h)) return;

    const float R = std::clamp(std::ceil(reach_px / dsf), 2.f, kMaxRadius);
    const float far_texels = R + 1.f;

    /* Every pass is clipped to the figures plus that reach. */
    const float pad = (R + 1.f) * dsf;
    const float fw = static_cast<float>(display_w);
    const float fh = static_cast<float>(display_h);

    const float rx0 = std::clamp(box.min_x - pad, 0.f, fw);
    const float ry0 = std::clamp(box.min_y - pad, 0.f, fh);
    const float rx1 = std::clamp(box.max_x + pad, 0.f, fw);
    const float ry1 = std::clamp(box.max_y + pad, 0.f, fh);
    if (rx1 - rx0 < 2.f || ry1 - ry0 < 2.f) return;

    const float u0 = rx0 / fw, v0 = ry0 / fh;
    const float u1 = rx1 / fw, v1 = ry1 / fh;

    const auto& g = ::vanta::al::gl::api();

    // Snapshot the state these passes perturb.
    GLint bound_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fb);
    GLint prev_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    const GLboolean blend_was = glIsEnabled(GL_BLEND);
    const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_was  = glIsEnabled(GL_CULL_FACE);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    /* Face culling would not actually break the mask -- three faces of a box
       tile its silhouette exactly, so either half of a cube covers the same
       texels as all six -- but it makes the answer depend on whoever set the
       state last, and a silhouette that is right by luck is not right. */
    glDisable(GL_CULL_FACE);

    const float texel_x = 1.f / static_cast<float>(buf_w);
    const float texel_y = 1.f / static_cast<float>(buf_h);

    // ── Pass 1: the union, as lit texels ────────────────────────────────────
    upload(xy, vertex_count);

    mask_.bind_draw();
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (fill_.bind()) {
        fill_.set("uInvSize", 1.f / fw, 1.f / fh);
        g.BindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, vertex_count);
        g.BindVertexArray(0);
    }

    // ── Pass 2a: horizontal distance ────────────────────────────────────────
    /* Cleared to "far" rather than to zero: pass 2b reads rows above and below
       the region, and a zero there would read as a lit texel, ringing the
       region's own border with an outline. */
    tmp_.bind_draw();
    glClearColor(1.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (dist_h_.bind()) {
        g.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mask_.color_texture_id());
        if (g.BindSampler) g.BindSampler(0, 0);
        dist_h_.set("uMask", 0);
        dist_h_.set("uTexel", texel_x, texel_y);
        dist_h_.set("uR", R);
        dist_h_.set("uMin", u0, v0);
        dist_h_.set("uMax", u1, v1);
        quad_.draw();
    }

    // ── Pass 2b: the field ──────────────────────────────────────────────────
    dist_.bind_draw();
    glClearColor(1.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (dist_v_.bind()) {
        g.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tmp_.color_texture_id());
        if (g.BindSampler) g.BindSampler(0, 0);
        dist_v_.set("uSrc", 0);
        dist_v_.set("uTexel", texel_x, texel_y);
        dist_v_.set("uR", R);
        dist_v_.set("uMin", u0, v0);
        dist_v_.set("uMax", u1, v1);
        quad_.draw();
    }

    // ── Pass 3: line + halo, onto the caller's framebuffer ──────────────────
    const unsigned int dest = composite_fbo > 0 ? composite_fbo
                                                : static_cast<unsigned int>(bound_fb);
    g.BindFramebuffer(GL_FRAMEBUFFER, dest);
    glViewport(0, 0, display_w, display_h);
    glEnable(GL_BLEND);

    if (draw_.bind()) {
        g.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dist_.color_texture_id());
        if (g.BindSampler) g.BindSampler(0, 0);
        draw_.set("uDist", 0);
        draw_.set("uFarPx",  far_texels * dsf);
        draw_.set("uBiasPx", dsf * 0.5f);
        draw_.set("uThick",  p.thickness_px);
        draw_.set("uGlow",   p.glow_px);
        draw_.set("uGlowAlpha", p.glow_alpha);
        draw_.set("uColor", p.col_r, p.col_g, p.col_b, p.col_a);
        draw_.set("uMin", u0, v0);
        draw_.set("uMax", u1, v1);

        /* HALO FIRST, screened. GL_ONE_MINUS_DST_COLOR is a legal GL 1.1
           source factor and needs nothing added to the loader.

           The chams layer does patch glBlendFunc, and it is benign here: its
           tracker early-returns while suppression is raised, which gl_hook does
           around this whole region, and it forwards unmodified regardless. This
           pass already called glBlendFunc before. */
        if (p.glow_px > 0.5f) {
            glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE);
            draw_.set("uPass", 0);
            quad_.draw();
        }

        /* LINE SECOND, so it sits in front of its own glow and its antialiased
           edge composites onto the halo rather than onto raw world.
           Premultiplied, which for src = colour * a is the same line the
           straight-alpha path drew — the line's blend was never the defect. */
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        draw_.set("uPass", 1);
        quad_.draw();
    }

    // Restore.
    g.UseProgram(static_cast<unsigned int>(prev_program));
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(bound_fb));
    glViewport(0, 0, display_w, display_h);
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    if (blend_was) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth_was) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull_was)  glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

void pipeline::destroy()
{
    const auto& g = ::vanta::al::gl::api();
    if (g.ok) {
        if (vbo_) { g.DeleteBuffers(1, &vbo_); vbo_ = 0; }
        if (vao_) { g.DeleteVertexArrays(1, &vao_); vao_ = 0; }
    }
    fill_.destroy();
    dist_h_.destroy();
    dist_v_.destroy();
    draw_.destroy();
    quad_.destroy();
    mask_.destroy();
    tmp_.destroy();
    dist_.destroy();

    shaders_tried_ = false;
    shaders_ok_    = false;
}

} // namespace vanta::modules::outline::gl
