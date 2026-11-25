// gl_ext.h — hand-rolled loader for the GL 3.0+ functions the arraylist needs.
//
// The rest of Dt gets by with plain <gl/GL.h> (GL 1.1) plus a couple of
// ad-hoc wglGetProcAddress lookups in gl_hook.cc. The arraylist pipeline is
// heavier: shaders, framebuffer objects, vertex-array objects, texture units.
// Rather than pulling in glad/glew, this loads just what we need on first use.
//
// Every symbol is namespaced under dt::al::gl to avoid clashing with the
// GL macros ImGui's bundled loader defines elsewhere.

#pragma once

#include <windows.h>
#include <gl/GL.h>
#include <cstdint>

// Types that the base <gl/GL.h> from the Windows SDK does not provide.
typedef char     GLchar;
typedef intptr_t GLintptr;
typedef ptrdiff_t GLsizeiptr;

// GL 2.0+ enums we reference explicitly (the SDK's GL.h stops at 1.1).
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

namespace dt::al::gl {

// ── Shader program ────────────────────────────────────────────────────────────
using PFN_glCreateShader        = GLuint  (APIENTRY*)(GLenum);
using PFN_glDeleteShader        = void    (APIENTRY*)(GLuint);
using PFN_glShaderSource        = void    (APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader       = void    (APIENTRY*)(GLuint);
using PFN_glGetShaderiv         = void    (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog    = void    (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glCreateProgram       = GLuint  (APIENTRY*)();
using PFN_glDeleteProgram       = void    (APIENTRY*)(GLuint);
using PFN_glAttachShader        = void    (APIENTRY*)(GLuint, GLuint);
using PFN_glLinkProgram         = void    (APIENTRY*)(GLuint);
using PFN_glGetProgramiv        = void    (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog   = void    (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glUseProgram          = void    (APIENTRY*)(GLuint);
using PFN_glGetUniformLocation  = GLint   (APIENTRY*)(GLuint, const GLchar*);
using PFN_glUniform1i           = void    (APIENTRY*)(GLint, GLint);
using PFN_glUniform1f           = void    (APIENTRY*)(GLint, GLfloat);
using PFN_glUniform2f           = void    (APIENTRY*)(GLint, GLfloat, GLfloat);
using PFN_glUniform3f           = void    (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using PFN_glUniform4f           = void    (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);

// ── Vertex arrays / buffers ───────────────────────────────────────────────────
using PFN_glGenVertexArrays     = void    (APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteVertexArrays  = void    (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindVertexArray     = void    (APIENTRY*)(GLuint);
using PFN_glGenBuffers          = void    (APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteBuffers       = void    (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindBuffer          = void    (APIENTRY*)(GLenum, GLuint);
using PFN_glBufferData          = void    (APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glEnableVertexAttribArray  = void (APIENTRY*)(GLuint);
using PFN_glDisableVertexAttribArray = void (APIENTRY*)(GLuint);
using PFN_glVertexAttribPointer      = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);

// ── Framebuffer objects ───────────────────────────────────────────────────────
using PFN_glGenFramebuffers          = void  (APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers       = void  (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindFramebuffer          = void  (APIENTRY*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D     = void  (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus   = GLenum(APIENTRY*)(GLenum);

// ── Texture unit ──────────────────────────────────────────────────────────────
using PFN_glActiveTexture       = void    (APIENTRY*)(GLenum);
using PFN_glBindSampler         = void    (APIENTRY*)(GLuint, GLuint);

// ── Function pointer table (populated by load()) ──────────────────────────────
struct funcs {
    PFN_glCreateShader        CreateShader        = nullptr;
    PFN_glDeleteShader        DeleteShader        = nullptr;
    PFN_glShaderSource        ShaderSource        = nullptr;
    PFN_glCompileShader       CompileShader       = nullptr;
    PFN_glGetShaderiv         GetShaderiv         = nullptr;
    PFN_glGetShaderInfoLog    GetShaderInfoLog    = nullptr;
    PFN_glCreateProgram       CreateProgram       = nullptr;
    PFN_glDeleteProgram       DeleteProgram       = nullptr;
    PFN_glAttachShader        AttachShader        = nullptr;
    PFN_glLinkProgram         LinkProgram         = nullptr;
    PFN_glGetProgramiv        GetProgramiv        = nullptr;
    PFN_glGetProgramInfoLog   GetProgramInfoLog   = nullptr;
    PFN_glUseProgram          UseProgram          = nullptr;
    PFN_glGetUniformLocation  GetUniformLocation  = nullptr;
    PFN_glUniform1i           Uniform1i           = nullptr;
    PFN_glUniform1f           Uniform1f           = nullptr;
    PFN_glUniform2f           Uniform2f           = nullptr;
    PFN_glUniform3f           Uniform3f           = nullptr;
    PFN_glUniform4f           Uniform4f           = nullptr;

    PFN_glGenVertexArrays     GenVertexArrays     = nullptr;
    PFN_glDeleteVertexArrays  DeleteVertexArrays  = nullptr;
    PFN_glBindVertexArray     BindVertexArray     = nullptr;
    PFN_glGenBuffers          GenBuffers          = nullptr;
    PFN_glDeleteBuffers       DeleteBuffers       = nullptr;
    PFN_glBindBuffer          BindBuffer          = nullptr;
    PFN_glBufferData          BufferData          = nullptr;
    PFN_glEnableVertexAttribArray  EnableVertexAttribArray  = nullptr;
    PFN_glDisableVertexAttribArray DisableVertexAttribArray = nullptr;
    PFN_glVertexAttribPointer      VertexAttribPointer      = nullptr;

    PFN_glGenFramebuffers          GenFramebuffers          = nullptr;
    PFN_glDeleteFramebuffers       DeleteFramebuffers       = nullptr;
    PFN_glBindFramebuffer          BindFramebuffer          = nullptr;
    PFN_glFramebufferTexture2D     FramebufferTexture2D     = nullptr;
    PFN_glCheckFramebufferStatus   CheckFramebufferStatus   = nullptr;

    PFN_glActiveTexture       ActiveTexture       = nullptr;
    PFN_glBindSampler         BindSampler         = nullptr;

    bool ok = false;
};

// Populates the table by asking wglGetProcAddress for every symbol. Idempotent —
// re-invoking after success is a no-op. On failure `ok` stays false and any
// caller that checks it should short-circuit rather than call through null.
const funcs& load();

// Convenience — returns the shared table without re-loading.
inline const funcs& api() { return load(); }

} // namespace dt::al::gl
