// al_gl.h — thin RAII wrappers around the GL 3 primitives the arraylist
// pipeline needs. Ports of AlShader / AlQuad / AlFbo from the Java source.
//
// Every ctor / method routes GL calls through dt::al::gl::api(), so the
// pipeline stays inert if the loader failed (the wrappers just report "not
// ready" instead of crashing on null function pointers).

#pragma once

#include "gl_ext.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace dt::al {

// ── AlShader ─────────────────────────────────────────────────────────────────
// A linked vertex + fragment program. Uniform locations are looked up lazily
// and memoised, matching the Java version.
class al_shader {
public:
    al_shader() = default;
    ~al_shader() { destroy(); }
    al_shader(const al_shader&)            = delete;
    al_shader& operator=(const al_shader&) = delete;

    // Compile + link from source strings. On failure `linked()` stays false and
    // subsequent bind()/set()/uniform() calls are silent no-ops.
    bool load(const char* vertex_src, const char* fragment_src, const char* debug_label);

    bool linked() const { return linked_; }
    void destroy();

    // Sets the program current. Returns false if the program never linked.
    bool bind();

    // Uniform setters — silently no-op if the shader is unlinked or the
    // uniform is optimised out (glGetUniformLocation returns -1).
    void set(const char* name, int value);
    void set(const char* name, float value);
    void set(const char* name, float x, float y);
    void set(const char* name, float x, float y, float z);
    void set(const char* name, float x, float y, float z, float w);
    void set_vec4_array(const char* name, const float* xyzw, int count);
    void set_float_array(const char* name, const float* v, int count);

private:
    int uniform(const char* name);

    unsigned int program_id_ = 0;
    unsigned int vertex_id_  = 0;
    unsigned int fragment_id_= 0;
    bool         linked_     = false;
    std::unordered_map<std::string, int> uniform_cache_{};
};

// ── AlQuad ───────────────────────────────────────────────────────────────────
// Fullscreen quad in clip space, drawn as two triangles from a VAO/VBO.
class al_quad {
public:
    al_quad() = default;
    ~al_quad() { destroy(); }
    al_quad(const al_quad&)            = delete;
    al_quad& operator=(const al_quad&) = delete;

    // Lazily creates the VAO/VBO on first call. Returns false if GL failed.
    bool ensure();
    void draw();
    void destroy();

private:
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    bool ready_ = false;
};

// ── AlFbo ────────────────────────────────────────────────────────────────────
// One RGBA colour-attachment framebuffer, resizable in-place.
class al_fbo {
public:
    al_fbo() = default;
    ~al_fbo() { destroy(); }
    al_fbo(const al_fbo&)            = delete;
    al_fbo& operator=(const al_fbo&) = delete;

    // (Re)allocate to the requested size. Returns false on GL failure or if
    // size < 1. Cheap when nothing changed — used every frame.
    bool ensure(int w, int h);

    // Set as the draw target and apply a matching viewport.
    void bind_draw();

    // glClearColor(0,0,0,0) + glClear(COLOR).
    void clear();

    void         bind_color_texture();
    unsigned int color_texture_id() const { return color_texture_id_; }
    unsigned int framebuffer_id()   const { return framebuffer_id_; }
    int          width()  const { return width_; }
    int          height() const { return height_; }
    bool         valid()  const { return framebuffer_id_ != 0 && color_texture_id_ != 0; }

    void destroy();

private:
    unsigned int framebuffer_id_  = 0;
    unsigned int color_texture_id_= 0;
    int          width_  = 0;
    int          height_ = 0;
};

} // namespace dt::al
