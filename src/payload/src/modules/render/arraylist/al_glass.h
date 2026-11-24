// al_glass.h — frosted-glass background pipeline (dual-Kawase blur inside a
// union of rounded rectangles), ported from Java's AlGlass.

#pragma once

#include "al_gl.h"

#include <vector>

namespace vanta::al {

// One rounded rectangle in framebuffer pixels, bottom-left origin, with four
// independent corner radii so vertically-adjacent rows can merge cleanly.
struct al_rect {
    float x{}, y{}, w{}, h{};
    float rTL{}, rTR{}, rBR{}, rBL{};

    al_rect() = default;
    al_rect(float x_, float y_, float w_, float h_, float r_tl, float r_tr, float r_br, float r_bl)
        : x(x_), y(y_), w(w_), h(h_), rTL(r_tl), rTR(r_tr), rBR(r_br), rBL(r_bl) {}
};

struct al_tint { float r, g, b, a; };

class al_glass {
public:
    static constexpr int max_rects = 64;

    al_glass() = default;
    ~al_glass() { destroy(); }
    al_glass(const al_glass&)            = delete;
    al_glass& operator=(const al_glass&) = delete;

    // True once the three shaders compile + link successfully.
    bool available();

    // Runs the pipeline: captures a bbox of the currently bound framebuffer,
    // dual-Kawase blurs the region, and composites the blurred glass inside
    // the rounded rects (plus outer drop shadow) back onto `composite_fbo`.
    //
    // All rectangles + bbox coords are in FRAMEBUFFER PIXELS, bottom-left
    // origin (matching gl_FragCoord). The caller passes the current framebuffer
    // as `composite_fbo`; pass 0 to fall back to whatever is bound.
    void render(const std::vector<al_rect>& rects,
                int display_w, int display_h,
                unsigned int composite_fbo,
                int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                int blur_passes,
                float brightness,
                al_tint tint,
                float edge_softness,
                float shadow_radius, float shadow_strength, float shadow_gamma,
                // Coverage of the whole pane, 1 = solid. This is the ONLY way to
                // fade a pane out: winding the tint alpha down instead leaves the
                // blurred scene composited at full coverage, which is an opaque
                // frosted rectangle rather than nothing. Defaulted, so the
                // callers that never fade do not have to know about it.
                float opacity = 1.0f, unsigned int source_fbo = 0);

    void destroy();

private:
    void ensure_shaders();
    void capture_scene(int region_w, int region_h, int src_x, int src_y, unsigned int source_fbo);
    void kawase_pass(al_shader& shader, al_fbo& target, unsigned int source_tex,
                     float texel_x, float texel_y, float offset);
    al_fbo* blur(int display_w, int display_h, int half_w, int half_h, int passes);
    void composite_glass(al_fbo* blurred, const std::vector<al_rect>& rects,
                         int display_w, int display_h,
                         int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                         float brightness, al_tint tint, float edge_softness,
                         float shadow_radius, float shadow_strength, float shadow_gamma,
                         float opacity);

    al_shader kawase_down_{};
    al_shader kawase_up_{};
    al_shader composite_{};
    bool      shaders_tried_ = false;
    bool      shaders_ok_    = false;

    al_quad   quad_{};
    al_fbo    fbo_a_{};
    al_fbo    fbo_b_{};

    unsigned int scene_texture_ = 0;
    int          scene_w_ = 0;
    int          scene_h_ = 0;
};

} // namespace vanta::al
