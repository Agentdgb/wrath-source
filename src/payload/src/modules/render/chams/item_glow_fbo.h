// item_glow_fbo.h — the ItemShader feature's OWN framebuffer target.
//
// The arraylist's al_fbo is an 8-bit (GL_RGBA8) colour target, which is fine for
// flat UI but wrong for a bloom: a smooth coverage gradient stored at 8 bits has
// only 256 levels, and the composite's pow()/intensity stretch turns each 1/255
// step into a visible concentric "line" (banding). This target is deliberately
// kept SEPARATE from the arraylist — it stores the blur in half-float (RGBA16F)
// so the gradient survives the whole ping-pong smoothly. Same method names as
// al_fbo so the glow code reads the same, but nothing here touches the arraylist.
//
// Routes GL through the shared vanta::al::gl loader (api()) exactly like the rest
// of the native layer; the loader is neutral infrastructure, not arraylist logic.

#pragma once

#include "modules/render/arraylist/gl_ext.h"   // shared GL 3.0+ loader: api()

// Float-format enums the shared loader doesn't already define.
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

namespace vanta::modules::itemshader {

// One RGBA16F colour-attachment framebuffer, resizable in-place. Half-float so a
// blurred gradient keeps far more than 256 levels — no 8-bit banding lines.
class glow_fbo {
public:
    glow_fbo() = default;
    ~glow_fbo() { destroy(); }
    glow_fbo(const glow_fbo&)            = delete;
    glow_fbo& operator=(const glow_fbo&) = delete;

    // (Re)allocate to the requested size. Cheap no-op when unchanged.
    bool ensure(int w, int h) {
        if (w < 1 || h < 1) return false;
        const auto& g = ::vanta::al::gl::api();
        if (!g.ok) return false;
        if (framebuffer_id_ != 0 && width_ == w && height_ == h) return true;

        destroy();
        width_ = w; height_ = h;

        glGenTextures(1, &color_texture_id_);
        glBindTexture(GL_TEXTURE_2D, color_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Half-float storage — the whole point of this separate target.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);

        g.GenFramebuffers(1, &framebuffer_id_);
        g.BindFramebuffer(GL_FRAMEBUFFER, framebuffer_id_);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_id_, 0);

        const bool complete = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        g.BindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (!complete) { destroy(); return false; }
        return true;
    }

    void bind_draw() {
        ::vanta::al::gl::api().BindFramebuffer(GL_FRAMEBUFFER, framebuffer_id_);
        glViewport(0, 0, width_, height_);
    }

    void clear() {
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    unsigned int color_texture_id() const { return color_texture_id_; }
    unsigned int framebuffer_id()   const { return framebuffer_id_; }
    int          width()  const { return width_; }
    int          height() const { return height_; }
    bool         valid()  const { return framebuffer_id_ != 0 && color_texture_id_ != 0; }

    void destroy() {
        const auto& g = ::vanta::al::gl::api();
        if (color_texture_id_) { glDeleteTextures(1, &color_texture_id_); color_texture_id_ = 0; }
        if (framebuffer_id_ && g.ok) { g.DeleteFramebuffers(1, &framebuffer_id_); framebuffer_id_ = 0; }
        width_ = 0; height_ = 0;
    }

private:
    unsigned int framebuffer_id_   = 0;
    unsigned int color_texture_id_ = 0;
    int          width_  = 0;
    int          height_ = 0;
};

} // namespace vanta::modules::itemshader
