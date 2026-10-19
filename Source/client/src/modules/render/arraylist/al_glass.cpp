#include "al_glass.h"
#include "shaders.h"
#include "util/Log.h"

#include <algorithm>

namespace vanta::al {

using gl::api;

void al_glass::ensure_shaders()
{
    if (shaders_tried_)
        return;
    shaders_tried_ = true;

    const bool a = kawase_down_.load(shaders::fullscreen_vsh, shaders::kawase_down_fsh, "kawase_down");
    const bool b = kawase_up_.load(shaders::fullscreen_vsh, shaders::kawase_up_fsh,   "kawase_up");
    const bool c = composite_.load(shaders::fullscreen_vsh, shaders::composite_fsh,   "composite");
    shaders_ok_ = a && b && c;
}

bool al_glass::available()
{
    ensure_shaders();
    return shaders_ok_;
}

void al_glass::destroy()
{
    const auto& g = api();
    if (scene_texture_ != 0 && g.ok) {
        glDeleteTextures(1, &scene_texture_);
        scene_texture_ = 0;
    }
    quad_.destroy();
    fbo_a_.destroy();
    fbo_b_.destroy();
    kawase_down_.destroy();
    kawase_up_.destroy();
    composite_.destroy();
    shaders_ok_    = false;
    shaders_tried_ = false;
}

void al_glass::capture_scene(int region_w, int region_h, int src_x, int src_y, unsigned int source_fbo)
{
    const auto& g = api();

    if (scene_texture_ == 0) {
        glGenTextures(1, &scene_texture_);
    }
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_texture_);

    if (scene_w_ != region_w || scene_h_ != region_h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, region_w, region_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        scene_w_ = region_w;
        scene_h_ = region_h;
    }

    /* Pin the read framebuffer + read buffer explicitly. During a GUI flush
       either may already point elsewhere, and glCopyTexSubImage2D from an
       unexpected source reads black instead of the world. */
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    glReadBuffer(source_fbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_x, src_y, region_w, region_h);
}

void al_glass::kawase_pass(al_shader& shader, al_fbo& target, unsigned int source_tex,
                           float texel_x, float texel_y, float offset)
{
    target.bind_draw();
    target.clear();
    if (!shader.bind())
        return;

    shader.set("uTex", 0);
    shader.set("uTexel", texel_x, texel_y);
    shader.set("uOffset", offset);

    const auto& g = api();
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_tex);
    if (g.BindSampler) g.BindSampler(0, 0);   // clear any foreign sampler object
    quad_.draw();
}

al_fbo* al_glass::blur(int display_w, int display_h, int half_w, int half_h, int passes)
{
    // Full-res scene -> half-res fbo_a.
    kawase_pass(kawase_down_, fbo_a_, scene_texture_,
                1.f / display_w, 1.f / display_h, 1.f);

    al_fbo* source = &fbo_a_;
    al_fbo* target = &fbo_b_;
    const int loops = std::clamp(passes, 1, 8);
    for (int i = 0; i < loops; ++i) {
        const float offset = 1.5f + static_cast<float>(i);
        kawase_pass(kawase_up_, *target, source->color_texture_id(),
                    1.f / half_w, 1.f / half_h, offset);
        std::swap(source, target);
    }
    return source;
}

void al_glass::composite_glass(al_fbo* blurred, const std::vector<al_rect>& rects,
                               int display_w, int display_h,
                               int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                               float brightness, al_tint tint, float edge_softness,
                               float shadow_radius, float shadow_strength, float shadow_gamma,
                               float opacity)
{
    if (!composite_.bind())
        return;

    const int count = std::min<int>(static_cast<int>(rects.size()), max_rects);
    float xywh[max_rects * 4]{};
    float radii[max_rects * 4]{};
    for (int i = 0; i < count; ++i) {
        const al_rect& r = rects[i];
        xywh [i*4+0] = r.x;   xywh [i*4+1] = r.y;
        xywh [i*4+2] = r.w;   xywh [i*4+3] = r.h;
        radii[i*4+0] = r.rTL; radii[i*4+1] = r.rTR;
        radii[i*4+2] = r.rBR; radii[i*4+3] = r.rBL;
    }

    composite_.set("uBlur", 0);
    composite_.set("uResolution", static_cast<float>(display_w), static_cast<float>(display_h));
    composite_.set("uBboxMin",    static_cast<float>(bbox_x),    static_cast<float>(bbox_y));
    composite_.set("uBboxSize",   static_cast<float>(bbox_w),    static_cast<float>(bbox_h));
    composite_.set("uRectCount",  count);
    composite_.set_vec4_array("uRects", xywh,  count);
    composite_.set_vec4_array("uRadii", radii, count);
    composite_.set("uBrightness", brightness);
    composite_.set("uTint", tint.r, tint.g, tint.b, tint.a);
    composite_.set("uEdge", edge_softness);
    composite_.set("uShadowRadius", shadow_radius);
    composite_.set("uShadowStrength", shadow_strength);
    composite_.set("uShadowGamma",  shadow_gamma);
    composite_.set("uOpacity",      opacity);

    const auto& g = api();
    g.ActiveTexture(GL_TEXTURE0);
    blurred->bind_color_texture();
    if (g.BindSampler) g.BindSampler(0, 0);
    quad_.draw();
}

void al_glass::render(const std::vector<al_rect>& rects,
                      int display_w, int display_h,
                      unsigned int composite_fbo,
                      int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                      int blur_passes,
                      float brightness,
                      al_tint tint,
                      float edge_softness,
                      float shadow_radius, float shadow_strength, float shadow_gamma,
                      float opacity, unsigned int source_fbo)
{
    ensure_shaders();
    if (!shaders_ok_ || rects.empty() || display_w < 2 || display_h < 2)
        return;
    if (!quad_.ensure())
        return;

    // Clamp bbox to the framebuffer.
    bbox_x = std::max(0, std::min(bbox_x, display_w - 2));
    bbox_y = std::max(0, std::min(bbox_y, display_h - 2));
    bbox_w = std::max(2, std::min(bbox_w, display_w - bbox_x));
    bbox_h = std::max(2, std::min(bbox_h, display_h - bbox_y));

    const int half_w = std::max(1, bbox_w / 2);
    const int half_h = std::max(1, bbox_h / 2);
    if (!fbo_a_.ensure(half_w, half_h) || !fbo_b_.ensure(half_w, half_h))
        return;

    const auto& g = api();

    // Snapshot the pieces of state we may perturb.
    GLint bound_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fb);
    GLint prev_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    const GLboolean blend_was  = glIsEnabled(GL_BLEND);
    const GLboolean depth_was  = glIsEnabled(GL_DEPTH_TEST);

    glDisable(GL_DEPTH_TEST);

    capture_scene(bbox_w, bbox_h, bbox_x, bbox_y, source_fbo);

    glDisable(GL_BLEND);
    al_fbo* blurred = blur(bbox_w, bbox_h, half_w, half_h, blur_passes);

    const unsigned int dest = composite_fbo > 0 ? composite_fbo
                                                : static_cast<unsigned int>(bound_fb);
    g.BindFramebuffer(GL_FRAMEBUFFER, dest);
    glViewport(0, 0, display_w, display_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    composite_glass(blurred, rects, display_w, display_h,
                    bbox_x, bbox_y, bbox_w, bbox_h,
                    brightness, tint, edge_softness,
                    shadow_radius, shadow_strength, shadow_gamma, opacity);

    // Restore state.
    g.UseProgram(static_cast<unsigned int>(prev_program));
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(bound_fb));
    glViewport(0, 0, display_w, display_h);
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (blend_was) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth_was) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

} // namespace vanta::al
