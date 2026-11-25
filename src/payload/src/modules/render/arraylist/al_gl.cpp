#include "al_gl.h"
#include "util/Log.h"

#include <cstring>

namespace dt::al {

using gl::api;

// ─── al_shader ──────────────────────────────────────────────────────────────

bool al_shader::load(const char* vertex_src, const char* fragment_src, const char* debug_label)
{
    const auto& g = api();
    if (!g.ok)
        return false;

    destroy();

    program_id_  = g.CreateProgram();
    vertex_id_   = g.CreateShader(GL_VERTEX_SHADER);
    fragment_id_ = g.CreateShader(GL_FRAGMENT_SHADER);

    if (program_id_ == 0 || vertex_id_ == 0 || fragment_id_ == 0) {
        destroy();
        return false;
    }

    const GLchar* v_srcs[1] = { vertex_src };
    const GLchar* f_srcs[1] = { fragment_src };

    g.ShaderSource(vertex_id_,   1, v_srcs, nullptr);
    g.ShaderSource(fragment_id_, 1, f_srcs, nullptr);
    g.CompileShader(vertex_id_);
    g.CompileShader(fragment_id_);

    GLint v_ok = 0, f_ok = 0;
    g.GetShaderiv(vertex_id_,   GL_COMPILE_STATUS, &v_ok);
    g.GetShaderiv(fragment_id_, GL_COMPILE_STATUS, &f_ok);

    if (!v_ok || !f_ok) {
        char log_buf[1024]{};
        GLsizei written = 0;
        if (!v_ok) {
            g.GetShaderInfoLog(vertex_id_, sizeof(log_buf), &written, log_buf);
            VLOG_ERR("arraylist", "vertex shader compile failed ({}): {}", debug_label, log_buf);
        }
        if (!f_ok) {
            std::memset(log_buf, 0, sizeof(log_buf));
            g.GetShaderInfoLog(fragment_id_, sizeof(log_buf), &written, log_buf);
            VLOG_ERR("arraylist", "fragment shader compile failed ({}): {}", debug_label, log_buf);
        }
        destroy();
        return false;
    }

    g.AttachShader(program_id_, vertex_id_);
    g.AttachShader(program_id_, fragment_id_);
    g.LinkProgram(program_id_);

    GLint link_ok = 0;
    g.GetProgramiv(program_id_, GL_LINK_STATUS, &link_ok);
    if (!link_ok) {
        char log_buf[1024]{};
        GLsizei written = 0;
        g.GetProgramInfoLog(program_id_, sizeof(log_buf), &written, log_buf);
        VLOG_ERR("arraylist", "shader link failed ({}): {}", debug_label, log_buf);
        destroy();
        return false;
    }

    linked_ = true;
    VLOG_INFO("arraylist", "shader linked: {}", debug_label);
    return true;
}

void al_shader::destroy()
{
    const auto& g = api();
    if (!g.ok)
        return;

    if (vertex_id_)   { g.DeleteShader(vertex_id_);   vertex_id_   = 0; }
    if (fragment_id_) { g.DeleteShader(fragment_id_); fragment_id_ = 0; }
    if (program_id_)  { g.DeleteProgram(program_id_); program_id_  = 0; }
    linked_ = false;
    uniform_cache_.clear();
}

bool al_shader::bind()
{
    if (!linked_)
        return false;
    api().UseProgram(program_id_);
    return true;
}

int al_shader::uniform(const char* name)
{
    if (!linked_)
        return -1;

    auto hit = uniform_cache_.find(name);
    if (hit != uniform_cache_.end())
        return hit->second;

    const int loc = api().GetUniformLocation(program_id_, name);
    uniform_cache_.emplace(name, loc);
    return loc;
}

void al_shader::set(const char* name, int value)
{
    int loc = uniform(name);
    if (loc >= 0) api().Uniform1i(loc, value);
}
void al_shader::set(const char* name, float value)
{
    int loc = uniform(name);
    if (loc >= 0) api().Uniform1f(loc, value);
}
void al_shader::set(const char* name, float x, float y)
{
    int loc = uniform(name);
    if (loc >= 0) api().Uniform2f(loc, x, y);
}
void al_shader::set(const char* name, float x, float y, float z)
{
    int loc = uniform(name);
    if (loc >= 0) api().Uniform3f(loc, x, y, z);
}
void al_shader::set(const char* name, float x, float y, float z, float w)
{
    int loc = uniform(name);
    if (loc >= 0) api().Uniform4f(loc, x, y, z, w);
}

void al_shader::set_vec4_array(const char* name, const float* xyzw, int count)
{
    /* Java does the same one-at-a-time upload — the LWJGL 2/3 API drift meant
       glUniform4fv wasn't available on both. Here we could use glUniform4fv,
       but the per-index [i] convention is what the shader expects and it lets
       us set fewer than the declared array size without extra padding. */
    if (!linked_ || xyzw == nullptr || count <= 0)
        return;

    char index_buf[64];
    for (int i = 0; i < count; ++i) {
        std::snprintf(index_buf, sizeof(index_buf), "%s[%d]", name, i);
        int loc = uniform(index_buf);
        if (loc >= 0)
            api().Uniform4f(loc, xyzw[i*4+0], xyzw[i*4+1], xyzw[i*4+2], xyzw[i*4+3]);
    }
}

void al_shader::set_float_array(const char* name, const float* v, int count)
{
    if (!linked_ || v == nullptr || count <= 0)
        return;

    char index_buf[64];
    for (int i = 0; i < count; ++i) {
        std::snprintf(index_buf, sizeof(index_buf), "%s[%d]", name, i);
        int loc = uniform(index_buf);
        if (loc >= 0)
            api().Uniform1f(loc, v[i]);
    }
}

// ─── al_quad ────────────────────────────────────────────────────────────────

bool al_quad::ensure()
{
    if (ready_)
        return true;

    const auto& g = api();
    if (!g.ok)
        return false;

    // Two triangles covering clip-space [-1,1] — same layout as the Java source.
    static const float vertices[12] = {
        -1.f, -1.f,   1.f, -1.f,   1.f, 1.f,
         1.f,  1.f,  -1.f,  1.f,  -1.f, -1.f
    };

    g.GenVertexArrays(1, &vao_);
    if (vao_ == 0)
        return false;

    g.BindVertexArray(vao_);
    g.GenBuffers(1, &vbo_);
    if (vbo_ == 0) {
        g.DeleteVertexArrays(1, &vao_);
        vao_ = 0;
        return false;
    }
    g.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    g.BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    g.BindVertexArray(0);
    g.BindBuffer(GL_ARRAY_BUFFER, 0);

    ready_ = true;
    return true;
}

void al_quad::draw()
{
    if (!ready_)
        return;
    const auto& g = api();
    g.BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    g.BindVertexArray(0);
}

void al_quad::destroy()
{
    const auto& g = api();
    if (!g.ok)
        return;

    if (vbo_) { g.DeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { g.DeleteVertexArrays(1, &vao_); vao_ = 0; }
    ready_ = false;
}

// ─── al_fbo ─────────────────────────────────────────────────────────────────

bool al_fbo::ensure(int w, int h)
{
    if (w < 1 || h < 1)
        return false;

    const auto& g = api();
    if (!g.ok)
        return false;

    if (framebuffer_id_ != 0 && width_ == w && height_ == h)
        return true;

    destroy();
    width_  = w;
    height_ = h;

    glGenTextures(1, &color_texture_id_);
    glBindTexture(GL_TEXTURE_2D, color_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    g.GenFramebuffers(1, &framebuffer_id_);
    g.BindFramebuffer(GL_FRAMEBUFFER, framebuffer_id_);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_id_, 0);

    const bool complete = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!complete) {
        destroy();
        return false;
    }
    return true;
}

void al_fbo::bind_draw()
{
    api().BindFramebuffer(GL_FRAMEBUFFER, framebuffer_id_);
    glViewport(0, 0, width_, height_);
}

void al_fbo::clear()
{
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void al_fbo::bind_color_texture()
{
    glBindTexture(GL_TEXTURE_2D, color_texture_id_);
}

void al_fbo::destroy()
{
    const auto& g = api();
    if (color_texture_id_) {
        glDeleteTextures(1, &color_texture_id_);
        color_texture_id_ = 0;
    }
    if (framebuffer_id_ && g.ok) {
        g.DeleteFramebuffers(1, &framebuffer_id_);
        framebuffer_id_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

} // namespace dt::al
