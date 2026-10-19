// item_shader_gl.cpp — the ItemShader feature's own native GL layer.
//
// Owns the whole first-person held-item glow: detection gate, capture of MC's own
// item draw into a half-float FBO, a separable Gaussian blur, and the additive
// composite of a soft colour-matched halo (plus a gentle inner bloom). It reaches
// GL only through the shared vanta::al::gl loader + raw GL 1.1, and brackets all of
// that in chams' suppression so the Chams state shadow is never polluted. It reads
// no Chams internals — chams passes in the per-draw classification it already has.

#include "item_shader_gl.h"
#include "item_glow_fbo.h"
#include "item_shader_log.h"
#include "chams_gl.h"                          // suppression bracket only
#include "modules/render/arraylist/al_gl.h"   // al_quad / al_shader (unmodified)
#include "modules/render/arraylist/gl_ext.h"  // shared GL 3.0+ loader: api()

#include <windows.h>
#include <gl/GL.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

// ── GL enums the base <gl/GL.h> / gl_ext.h don't already provide ──────────────
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_SAMPLER_BINDING
#define GL_SAMPLER_BINDING 0x8919
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_UNIFORM_BLOCK_BINDING
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif
#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif
#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif
#ifndef GL_SHADER_SOURCE_LENGTH
#define GL_SHADER_SOURCE_LENGTH 0x8B88
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

namespace vanta::modules::itemshader {
namespace {

namespace algl = ::vanta::al::gl;

// ── Suppression bracket ───────────────────────────────────────────────────────
// While raised, chams' hooked state setters skip their shadow update (but still
// call the original), so our GL never corrupts the Chams shadow. We self-snapshot
// and restore the live GL state around every pass, so chams sees the world exactly
// as it left it.
struct suppress_scope {
    bool prev;
    suppress_scope()  : prev(::vanta::modules::chams::gl::exchange_suppressed(true)) {}
    ~suppress_scope() { ::vanta::modules::chams::gl::set_suppressed(prev); }
};

// ── Own GL function pointers (resolved once, on a live context) ────────────────
using PFN_CreateShader            = GLuint (APIENTRY*)(GLenum);
using PFN_ShaderSource            = void   (APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_CompileShader           = void   (APIENTRY*)(GLuint);
using PFN_GetShaderiv             = void   (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetShaderInfoLog        = void   (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_GetShaderSource         = void   (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_GetAttachedShaders      = void   (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLuint*);
using PFN_CreateProgram           = GLuint (APIENTRY*)();
using PFN_AttachShader            = void   (APIENTRY*)(GLuint, GLuint);
using PFN_LinkProgram             = void   (APIENTRY*)(GLuint);
using PFN_GetProgramiv            = void   (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_GetProgramInfoLog       = void   (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_DeleteShader            = void   (APIENTRY*)(GLuint);
using PFN_GetUniformLocation      = GLint  (APIENTRY*)(GLuint, const GLchar*);
using PFN_Uniform1f               = void   (APIENTRY*)(GLint, GLfloat);
using PFN_Uniform2f               = void   (APIENTRY*)(GLint, GLfloat, GLfloat);
using PFN_GetUniformBlockIndex    = GLuint (APIENTRY*)(GLuint, const GLchar*);
using PFN_GetActiveUniformBlockiv = void   (APIENTRY*)(GLuint, GLuint, GLenum, GLint*);
using PFN_UniformBlockBinding     = void   (APIENTRY*)(GLuint, GLuint, GLuint);
using PFN_BlendFuncSeparate       = void   (APIENTRY*)(GLenum, GLenum, GLenum, GLenum);

PFN_CreateShader            f_CreateShader            = nullptr;
PFN_ShaderSource            f_ShaderSource            = nullptr;
PFN_CompileShader           f_CompileShader           = nullptr;
PFN_GetShaderiv             f_GetShaderiv             = nullptr;
PFN_GetShaderInfoLog        f_GetShaderInfoLog        = nullptr;
PFN_GetShaderSource         f_GetShaderSource         = nullptr;
PFN_GetAttachedShaders      f_GetAttachedShaders      = nullptr;
PFN_CreateProgram           f_CreateProgram           = nullptr;
PFN_AttachShader            f_AttachShader            = nullptr;
PFN_LinkProgram             f_LinkProgram             = nullptr;
PFN_GetProgramiv            f_GetProgramiv            = nullptr;
PFN_GetProgramInfoLog       f_GetProgramInfoLog       = nullptr;
PFN_DeleteShader            f_DeleteShader            = nullptr;
PFN_GetUniformLocation      f_GetUniformLocation      = nullptr;
PFN_Uniform1f               f_Uniform1f               = nullptr;
PFN_Uniform2f               f_Uniform2f               = nullptr;
PFN_GetUniformBlockIndex    f_GetUniformBlockIndex    = nullptr;
PFN_GetActiveUniformBlockiv f_GetActiveUniformBlockiv = nullptr;
PFN_UniformBlockBinding     f_UniformBlockBinding     = nullptr;
PFN_BlendFuncSeparate       f_BlendFuncSeparate       = nullptr;

bool g_gl_resolved = false;

void* resolve_gl(const char* name)
{
    if (void* p = reinterpret_cast<void*>(wglGetProcAddress(name))) {
        const intptr_t v = reinterpret_cast<intptr_t>(p);
        if (v != 0 && v != 1 && v != 2 && v != 3 && v != -1) return p;
    }
    if (HMODULE gl = GetModuleHandleA("opengl32.dll"))
        return reinterpret_cast<void*>(GetProcAddress(gl, name));
    return nullptr;
}

bool resolve_all()
{
    if (g_gl_resolved) return f_CreateProgram != nullptr;
    g_gl_resolved = true;
    f_CreateShader            = reinterpret_cast<PFN_CreateShader>(resolve_gl("glCreateShader"));
    f_ShaderSource            = reinterpret_cast<PFN_ShaderSource>(resolve_gl("glShaderSource"));
    f_CompileShader           = reinterpret_cast<PFN_CompileShader>(resolve_gl("glCompileShader"));
    f_GetShaderiv             = reinterpret_cast<PFN_GetShaderiv>(resolve_gl("glGetShaderiv"));
    f_GetShaderInfoLog        = reinterpret_cast<PFN_GetShaderInfoLog>(resolve_gl("glGetShaderInfoLog"));
    f_GetShaderSource         = reinterpret_cast<PFN_GetShaderSource>(resolve_gl("glGetShaderSource"));
    f_GetAttachedShaders      = reinterpret_cast<PFN_GetAttachedShaders>(resolve_gl("glGetAttachedShaders"));
    f_CreateProgram           = reinterpret_cast<PFN_CreateProgram>(resolve_gl("glCreateProgram"));
    f_AttachShader            = reinterpret_cast<PFN_AttachShader>(resolve_gl("glAttachShader"));
    f_LinkProgram             = reinterpret_cast<PFN_LinkProgram>(resolve_gl("glLinkProgram"));
    f_GetProgramiv            = reinterpret_cast<PFN_GetProgramiv>(resolve_gl("glGetProgramiv"));
    f_GetProgramInfoLog       = reinterpret_cast<PFN_GetProgramInfoLog>(resolve_gl("glGetProgramInfoLog"));
    f_DeleteShader            = reinterpret_cast<PFN_DeleteShader>(resolve_gl("glDeleteShader"));
    f_GetUniformLocation      = reinterpret_cast<PFN_GetUniformLocation>(resolve_gl("glGetUniformLocation"));
    f_Uniform1f               = reinterpret_cast<PFN_Uniform1f>(resolve_gl("glUniform1f"));
    f_Uniform2f               = reinterpret_cast<PFN_Uniform2f>(resolve_gl("glUniform2f"));
    f_GetUniformBlockIndex    = reinterpret_cast<PFN_GetUniformBlockIndex>(resolve_gl("glGetUniformBlockIndex"));
    f_GetActiveUniformBlockiv = reinterpret_cast<PFN_GetActiveUniformBlockiv>(resolve_gl("glGetActiveUniformBlockiv"));
    f_UniformBlockBinding     = reinterpret_cast<PFN_UniformBlockBinding>(resolve_gl("glUniformBlockBinding"));
    f_BlendFuncSeparate       = reinterpret_cast<PFN_BlendFuncSeparate>(resolve_gl("glBlendFuncSeparate"));
    const bool ok = f_CreateShader && f_ShaderSource && f_CompileShader && f_CreateProgram
        && f_AttachShader && f_LinkProgram && f_GetProgramiv && f_GetUniformLocation
        && f_GetUniformBlockIndex && f_GetActiveUniformBlockiv && f_UniformBlockBinding;
    ITEMSHADER_LOG("resolve GL: %s (createProgram=%p uniformBlockBinding=%p getBlockIndex=%p)",
                   ok ? "OK" : "MISSING SYMBOLS", (void*)f_CreateProgram,
                   (void*)f_UniformBlockBinding, (void*)f_GetUniformBlockIndex);
    return ok;
}

// ── Feature state ─────────────────────────────────────────────────────────────
std::atomic<bool> g_wanted{ false };
std::atomic<bool> g_effect{ false };
std::atomic<bool> g_debug{ false };
std::atomic<bool> g_rendering_fp{ false };

std::atomic<GLuint>  g_item_prog{ 0 };
std::atomic<GLuint>  g_item_tex{ 0 };
std::atomic<GLsizei> g_item_count{ 0 };
std::atomic<int>     g_dump_left{ 4 };
std::atomic<GLuint>  g_dumped_prog{ 0 };

std::atomic<unsigned long long> c_render_calls{ 0 };
std::atomic<unsigned long long> c_item_draws{ 0 };
std::atomic<unsigned long long> c_render_calls_frame{ 0 };
std::atomic<unsigned long long> c_item_draws_frame{ 0 };
std::atomic<unsigned long long> c_window_draws_frame{ 0 };

// HUD latch (render thread): once the world's hand/item has been captured and the
// first 2D HUD draw follows, stop capturing so the hotbar/inventory item models —
// 3D and in the same window — are left alone. Reset each frame.
thread_local bool t_captured_any = false;
thread_local bool t_hud_started  = false;

// Capture program (reuses MC's item geometry + transform UBOs).
std::atomic<bool> g_prog_ready{ false };
std::atomic<bool> g_prog_failed{ false };
GLuint g_capture_prog = 0;
GLint  g_u_alpha = -1, g_u_offset = -1;
std::atomic<float> g_tap_intensity{ 0.045f }, g_tap_thickness{ 0.020f };

// FBO bloom objects. Heap-allocated and never freed on purpose (running GL dtors
// at DLL unload is a known crash source; leaking a few GL objects at exit is safe).
glow_fbo*             g_glow_src  = nullptr;   // sharp coloured capture (the mask)
glow_fbo*             g_glow_a    = nullptr;   // ping
glow_fbo*             g_glow_b    = nullptr;   // pong (final blurred)
::vanta::al::al_quad*   g_quad    = nullptr;
::vanta::al::al_shader* g_blur    = nullptr;
::vanta::al::al_shader* g_comp    = nullptr;
std::atomic<bool> g_bloom_ready{ false };
std::atomic<bool> g_bloom_failed{ false };
bool g_captured = false;   // a fresh capture happened this frame
int  g_idle     = 99;      // frames since the last capture

// ── Shaders ───────────────────────────────────────────────────────────────────
constexpr const char* kQuadVSH =
    "#version 330\n"
    "layout(location = 0) in vec2 aPos;\n"
    "out vec2 vUv;\n"
    "void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

// Separable 13-tap Gaussian (sigma ~2.6 taps). Wide + dense so each pass samples
// smoothly. Stored in RGBA16F (glow_fbo) so the gradient never bands.
constexpr const char* kBlurFSH =
    "#version 330\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uDir;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    vec4 c = texture(uTex, vUv) * 0.155280;\n"
    "    c += (texture(uTex, vUv + uDir * 1.0) + texture(uTex, vUv - uDir * 1.0)) * 0.144210;\n"
    "    c += (texture(uTex, vUv + uDir * 2.0) + texture(uTex, vUv - uDir * 2.0)) * 0.115513;\n"
    "    c += (texture(uTex, vUv + uDir * 3.0) + texture(uTex, vUv - uDir * 3.0)) * 0.079800;\n"
    "    c += (texture(uTex, vUv + uDir * 4.0) + texture(uTex, vUv - uDir * 4.0)) * 0.047560;\n"
    "    c += (texture(uTex, vUv + uDir * 5.0) + texture(uTex, vUv - uDir * 5.0)) * 0.024440;\n"
    "    c += (texture(uTex, vUv + uDir * 6.0) + texture(uTex, vUv - uDir * 6.0)) * 0.010850;\n"
    "    o = c;\n"
    "}\n";

// Composite: outer halo (blur spilled OUTSIDE the item) + a gentle inner bloom
// (blurred colour bleeding softly INSIDE the silhouette). Additive over the frame.
constexpr const char* kCompFSH =
    "#version 330\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uBlur;\n"   // blurred, colour-premultiplied coverage
    "uniform sampler2D uMask;\n"   // sharp raw capture (the item)
    "uniform float uIntensity;\n"  // outer halo strength
    "uniform float uInner;\n"      // inner bloom strength
    "out vec4 o;\n"
    "void main() {\n"
    "    vec4 b = texture(uBlur, vUv);\n"
    "    float core = texture(uMask, vUv).a;\n"
    "    vec3 col = (b.a > 0.001) ? (b.rgb / b.a) : vec3(0.0);\n"
    // Outer halo: coverage outside the item, faded gently across the silhouette.
    "    float ring = clamp(b.a - core, 0.0, 1.0) * (1.0 - smoothstep(0.35, 1.0, core));\n"
    "    ring = pow(ring, 0.72);\n"
    // Inner bloom: soft blurred glow INSIDE the item, strongest where coverage is
    // full, so the held item itself gets a subtle lit-from-within look.
    "    float inner = core * uInner * b.a;\n"
    "    o = vec4(col * (ring * uIntensity + inner), 1.0);\n"
    "}\n";

// ── Capture program (built once we've seen MC's own item program) ─────────────
GLuint compile_shader(GLenum type, const char* src, const char* label)
{
    GLuint sh = f_CreateShader(type);
    if (!sh) return 0;
    const GLchar* arr[1] = { src };
    f_ShaderSource(sh, 1, arr, nullptr);
    f_CompileShader(sh);
    GLint ok = 0;
    f_GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024] = {0};
        GLsizei n = 0;
        if (f_GetShaderInfoLog) f_GetShaderInfoLog(sh, (GLsizei)sizeof(buf), &n, buf);
        ITEMSHADER_LOG("compile FAILED (%s): %s", label, buf);
        if (f_DeleteShader) f_DeleteShader(sh);
        return 0;
    }
    return sh;
}

void match_block(GLuint mc_prog, const char* name)
{
    const GLuint mc_idx = f_GetUniformBlockIndex(mc_prog, name);
    const GLuint my_idx = f_GetUniformBlockIndex(g_capture_prog, name);
    if (mc_idx == 0xFFFFFFFFu || my_idx == 0xFFFFFFFFu) {
        ITEMSHADER_LOG("block '%s' NOT FOUND (mcIdx=%u myIdx=%u)", name, mc_idx, my_idx);
        return;
    }
    GLint binding = 0;
    f_GetActiveUniformBlockiv(mc_prog, mc_idx, GL_UNIFORM_BLOCK_BINDING, &binding);
    f_UniformBlockBinding(g_capture_prog, my_idx, (GLuint)binding);
    ITEMSHADER_LOG("block '%s' -> binding %d (mcIdx=%u myIdx=%u)", name, binding, mc_idx, my_idx);
}

void build_capture_program()
{
    if (g_prog_ready.load(std::memory_order_relaxed)) return;
    if (g_prog_failed.load(std::memory_order_relaxed)) return;
    const GLuint mc_prog = g_item_prog.load(std::memory_order_relaxed);
    if (!mc_prog) return;
    if (!resolve_all()) { g_prog_failed.store(true, std::memory_order_relaxed); return; }

    static const char* VSH =
        "#version 330\n"
        "layout(std140) uniform DynamicTransforms {\n"
        "    mat4 ModelViewMat; vec4 ColorModulator; vec3 ModelOffset; mat4 TextureMat;\n"
        "};\n"
        "layout(std140) uniform Projection { mat4 ProjMat; };\n"
        "layout(location = 0) in vec3 Position;\n"
        "layout(location = 2) in vec2 UV0;\n"
        "uniform vec2 u_offset;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    vec4 p = ProjMat * ModelViewMat * vec4(Position, 1.0);\n"
        "    p.xy += u_offset * p.w;\n"
        "    gl_Position = p;\n"
        "    v_uv = UV0;\n"
        "}\n";
    static const char* FSH =
        "#version 330\n"
        "uniform sampler2D Sampler0;\n"
        "uniform float u_alpha;\n"
        "in vec2 v_uv;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec4 t = texture(Sampler0, v_uv);\n"
        "    if (t.a < 0.1) discard;\n"
        "    vec3 c = t.rgb * 1.6 + 0.22;\n"
        "    fragColor = vec4(c * u_alpha, 1.0);\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VSH, "vertex");
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FSH, "fragment");
    if (!vs || !fs) {
        if (vs && f_DeleteShader) f_DeleteShader(vs);
        if (fs && f_DeleteShader) f_DeleteShader(fs);
        g_prog_failed.store(true, std::memory_order_relaxed);
        return;
    }
    GLuint prog = f_CreateProgram();
    if (!prog) { g_prog_failed.store(true, std::memory_order_relaxed); return; }
    f_AttachShader(prog, vs);
    f_AttachShader(prog, fs);
    f_LinkProgram(prog);
    GLint linked = 0;
    f_GetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (f_DeleteShader) { f_DeleteShader(vs); f_DeleteShader(fs); }
    if (!linked) {
        char buf[1024] = {0};
        GLsizei n = 0;
        if (f_GetProgramInfoLog) f_GetProgramInfoLog(prog, (GLsizei)sizeof(buf), &n, buf);
        ITEMSHADER_LOG("link FAILED: %s", buf);
        g_prog_failed.store(true, std::memory_order_relaxed);
        return;
    }
    g_capture_prog = prog;
    g_u_alpha  = f_GetUniformLocation(prog, "u_alpha");
    g_u_offset = f_GetUniformLocation(prog, "u_offset");
    match_block(mc_prog, "DynamicTransforms");
    match_block(mc_prog, "Projection");
    ::vanta::desklog::gl_check(::vanta::modules::itemshader::log_ch(), "build_capture_program");
    g_prog_ready.store(true, std::memory_order_relaxed);
    ITEMSHADER_LOG("=== CAPTURE PROGRAM READY === prog=%u u_alpha=%d u_offset=%d (from MC prog %u)",
                   prog, g_u_alpha, g_u_offset, mc_prog);
}

// ── Blur/composite programs + fullscreen quad ─────────────────────────────────
void build_bloom()
{
    if (g_bloom_ready.load(std::memory_order_relaxed)) return;
    if (g_bloom_failed.load(std::memory_order_relaxed)) return;
    algl::load();
    if (!algl::api().ok) { g_bloom_failed.store(true, std::memory_order_relaxed);
        ITEMSHADER_LOG("bloom: al::gl loader not ready — using tap fallback"); return; }

    suppress_scope ss;
    if (!g_glow_src) g_glow_src = new glow_fbo();
    if (!g_glow_a)   g_glow_a   = new glow_fbo();
    if (!g_glow_b)   g_glow_b   = new glow_fbo();
    if (!g_quad)     g_quad     = new ::vanta::al::al_quad();
    if (!g_blur)     g_blur     = new ::vanta::al::al_shader();
    if (!g_comp)     g_comp     = new ::vanta::al::al_shader();

    bool ok = g_quad->ensure();
    ok = ok && g_blur->load(kQuadVSH, kBlurFSH, "itemglow-blur");
    ok = ok && g_comp->load(kQuadVSH, kCompFSH, "itemglow-composite");
    if (ok) { g_bloom_ready.store(true, std::memory_order_relaxed);
              ITEMSHADER_LOG("=== GLOW (FBO bloom) READY ==="); }
    else    { g_bloom_failed.store(true, std::memory_order_relaxed);
              ITEMSHADER_LOG("bloom: program/quad build FAILED — using tap fallback"); }
}

// ── Capture: redirect MC's own item draw into g_glow_src ──────────────────────
void capture(const std::function<void()>& redraw)
{
    if (!g_bloom_ready.load(std::memory_order_relaxed)) return;

    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    const int w = vp[2], h = vp[3];
    if (w < 2 || h < 2) return;

    suppress_scope ss;
    // The sharp capture (mask/core) stays FULL resolution so the item's silhouette
    // edge is crisp; the blur ping-pong targets are HALF resolution — a wide soft
    // glow is visually identical downsampled, at ~1/4 the blur cost. The blur steps
    // are in UV (0..1 across the screen), so the on-screen blur RADIUS is unchanged
    // by the target's resolution — nothing else needs adjusting for the look to
    // stay identical.
    const int hw = (w / 2) > 1 ? (w / 2) : 1;
    const int hh = (h / 2) > 1 ? (h / 2) : 1;
    const bool ok = g_glow_src->ensure(w, h) && g_glow_a->ensure(hw, hh) && g_glow_b->ensure(hw, hh);
    if (g_debug.load(std::memory_order_relaxed))
        ITEMSHADER_RL(1000, "capture: vp=%dx%d half=%dx%d ensure=%d srcFbo=%u srcTex=%u", w, h, hw, hh, (int)ok,
                      g_glow_src ? g_glow_src->framebuffer_id() : 0u,
                      g_glow_src ? g_glow_src->color_texture_id() : 0u);
    if (!ok) return;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    const bool scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    const bool depth_on   = glIsEnabled(GL_DEPTH_TEST);
    const bool blend_on   = glIsEnabled(GL_BLEND);
    if (scissor_on) glDisable(GL_SCISSOR_TEST);

    g_glow_src->bind_draw();
    if (!g_captured) g_glow_src->clear();

    // The item shader already has its program, UBOs, attributes and texture bound
    // for this exact draw; we only change the render target.
    if (depth_on) glDisable(GL_DEPTH_TEST);
    if (blend_on) glDisable(GL_BLEND);
    redraw();
    if (blend_on) glEnable(GL_BLEND);
    if (depth_on) glEnable(GL_DEPTH_TEST);

    if (scissor_on) glEnable(GL_SCISSOR_TEST);
    algl::api().BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    g_captured = true;
}

// ── Blur the capture and composite the glow onto the frame (FBO 0) ────────────
void blur_and_composite()
{
    if (!g_bloom_ready.load(std::memory_order_relaxed)) return;
    const int w = g_glow_src->width(), h = g_glow_src->height();
    if (w < 2 || h < 2) return;

    // Composite EVERY frame (not only on a fresh capture): the 1.21.9+ queue emits
    // the item only some frames, and skipping the others made the glow strobe. We
    // re-blur on a fresh capture and reuse the last blur otherwise; once the item
    // has been gone a few frames we stop (it was put away).
    const bool fresh = g_captured;
    g_captured = false;
    if (fresh) g_idle = 0; else ++g_idle;
    if (g_idle > 3) return;

    suppress_scope ss;
    const auto& g = algl::api();

    // Snapshot the live GL we will perturb.
    GLint prevFbo = 0, prevProg = 0, prevActive = GL_TEXTURE0, vp[4] = { 0, 0, w, h };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, vp);
    const bool blend_was = glIsEnabled(GL_BLEND);
    const bool depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLint bfSrcRGB = GL_ONE, bfDstRGB = GL_ZERO, bfSrcA = GL_ONE, bfDstA = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bfSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &bfDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bfSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bfDstA);
    GLint tex0 = 0, tex1 = 0, samp0 = 0, samp1 = 0;
    g.ActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp0);
    g.ActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp1);

    if (g_debug.load(std::memory_order_relaxed))
        ITEMSHADER_RL(1000, "composite: w=%d h=%d fresh=%d idle=%d srcTex=%u blurTex=%u prevFbo=%d",
                      w, h, (int)fresh, g_idle, g_glow_src->color_texture_id(),
                      g_glow_b->color_texture_id(), prevFbo);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    if (fresh) {
        const float tx = 1.0f / (float)w;
        const float ty = 1.0f / (float)h;
        auto blur_pass = [&](glow_fbo* dst, glow_fbo* src, float dx, float dy) {
            dst->bind_draw();
            dst->clear();
            g_blur->bind();
            g.ActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src->color_texture_id());
            if (g.BindSampler) g.BindSampler(0, 0);
            g_blur->set("uTex", 0);
            g_blur->set("uDir", dx, dy);
            g_quad->draw();
        };
        // Total blur = convolution of every pass, ending on FINE passes so the wide
        // early passes' sparse-tap ripple gets smoothed away. First pass reads the
        // sharp capture; the rest read the running blur. Final result in g_glow_b.
        const float sp[6] = { 10.f, 10.f, 8.f, 6.f, 4.f, 3.f };
        blur_pass(g_glow_a, g_glow_src, tx * sp[0], 0.f);  blur_pass(g_glow_b, g_glow_a, 0.f, ty * sp[0]);
        for (int i = 1; i < 6; ++i) {
            blur_pass(g_glow_a, g_glow_b, tx * sp[i], 0.f);
            blur_pass(g_glow_b, g_glow_a, 0.f, ty * sp[i]);
        }
    }

    // Composite onto FBO 0 — the buffer wglSwapBuffers presents. Additive.
    g.BindFramebuffer(GL_FRAMEBUFFER, 0u);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    g_comp->bind();
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_glow_b->color_texture_id());
    if (g.BindSampler) g.BindSampler(0, 0);
    g_comp->set("uBlur", 0);
    g.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_glow_src->color_texture_id());
    if (g.BindSampler) g.BindSampler(1, 0);
    g_comp->set("uMask", 1);
    g_comp->set("uIntensity", 3.4f);   // outer halo — "más glow"
    g_comp->set("uInner", 0.45f);      // inner bloom inside the item
    g_quad->draw();

    // Restore exactly what MC had, so its cached bindings stay valid.
    g.ActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (GLuint)tex1);
    if (g.BindSampler) g.BindSampler(1, (GLuint)samp1);
    g.ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    if (g.BindSampler) g.BindSampler(0, (GLuint)samp0);
    g.ActiveTexture((GLenum)prevActive);
    if (f_BlendFuncSeparate) f_BlendFuncSeparate((GLenum)bfSrcRGB, (GLenum)bfDstRGB,
                                                 (GLenum)bfSrcA, (GLenum)bfDstA);
    if (!blend_was) glDisable(GL_BLEND); else glEnable(GL_BLEND);
    if (depth_was)  glEnable(GL_DEPTH_TEST);
    g.UseProgram((GLuint)prevProg);
    g.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    const int errs = ::vanta::desklog::gl_check(::vanta::modules::itemshader::log_ch(), "blur_and_composite");
    if (g_debug.load(std::memory_order_relaxed))
        ITEMSHADER_RL(1000, "composite: DONE (gl errors drained=%d)", errs);
}

// ── Fallback tap outline (only if the FBO bloom never built) ──────────────────
void tap_outline(const std::function<void()>& redraw)
{
    if (!g_prog_ready.load(std::memory_order_relaxed)) return;

    suppress_scope ss;
    const auto& g = algl::api();
    GLint prevProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    const bool blend_was = glIsEnabled(GL_BLEND);
    GLint bfSrcRGB = GL_ONE, bfDstRGB = GL_ZERO, bfSrcA = GL_ONE, bfDstA = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bfSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &bfDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bfSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bfDstA);

    g.UseProgram(g_capture_prog);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);

    const float maxR  = g_tap_thickness.load(std::memory_order_relaxed);
    const float baseA = g_tap_intensity.load(std::memory_order_relaxed);
    constexpr int   kSamples = 110;
    constexpr float kGolden  = 2.39996323f;
    constexpr float kPi = 3.14159265f;
    for (int i = 0; i < kSamples; ++i) {
        const float t   = ((float)i + 0.5f) / (float)kSamples;
        const float rad = maxR * std::sqrt(t);
        const float ang = (float)i * kGolden;
        const float fall = 0.5f * (1.0f + std::cos(kPi * t));
        if (f_Uniform1f && g_u_alpha >= 0) f_Uniform1f(g_u_alpha, baseA * fall);
        if (f_Uniform2f && g_u_offset >= 0)
            f_Uniform2f(g_u_offset, std::cos(ang) * rad, std::sin(ang) * rad);
        redraw();
    }

    glDepthMask(GL_TRUE);
    if (f_BlendFuncSeparate) f_BlendFuncSeparate((GLenum)bfSrcRGB, (GLenum)bfDstRGB,
                                                 (GLenum)bfSrcA, (GLenum)bfDstA);
    if (!blend_was) glDisable(GL_BLEND);
    g.UseProgram((GLuint)prevProg);
}

// ── One-shot dump of MC's own first-person item shader GLSL (debug) ───────────
void dump_mc_shader()
{
    if (!g_wanted.load(std::memory_order_relaxed)) return;
    if (!g_debug.load(std::memory_order_relaxed))  return;
    if (g_dump_left.load(std::memory_order_relaxed) <= 0) return;
    if (!f_GetAttachedShaders || !f_GetShaderiv || !f_GetShaderSource) return;
    const GLuint prog = g_item_prog.load(std::memory_order_relaxed);
    if (!prog || g_dumped_prog.load(std::memory_order_relaxed) == prog) return;
    g_dumped_prog.store(prog, std::memory_order_relaxed);
    g_dump_left.fetch_sub(1, std::memory_order_relaxed);

    GLuint shaders[8] = {0};
    GLsizei n = 0;
    f_GetAttachedShaders(prog, 8, &n, shaders);
    ITEMSHADER_LOG("=== FP-ITEM SHADER DUMP === program=%u tex0=%u count=%d shaders=%d",
                   (unsigned)prog, (unsigned)g_item_tex.load(std::memory_order_relaxed),
                   (int)g_item_count.load(std::memory_order_relaxed), (int)n);
    for (GLsizei i = 0; i < n; ++i) {
        GLint len = 0;
        f_GetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &len);
        if (len <= 0) continue;
        if (len > (1 << 14)) len = (1 << 14);
        std::string src((size_t)len, '\0');
        GLsizei got = 0;
        f_GetShaderSource(shaders[i], len, &got, src.data());
        const size_t chunk = 900;
        for (size_t off = 0; off < src.size(); off += chunk) {
            const size_t rem = src.size() - off;
            const int m = (int)(rem < chunk ? rem : chunk);
            ITEMSHADER_LOG("[prog=%u shader=%u part=%zu]: %.*s",
                           (unsigned)prog, (unsigned)shaders[i], off / chunk, m, src.data() + off);
        }
    }
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────
void set_wanted(bool on)  { g_wanted.store(on, std::memory_order_release); }
bool wanted()             { return g_wanted.load(std::memory_order_acquire); }
void set_effect(bool on)  { g_effect.store(on, std::memory_order_relaxed); }
void set_debug(bool on)   { g_debug.store(on, std::memory_order_relaxed); }
void set_rendering_firstperson(bool on) { g_rendering_fp.store(on, std::memory_order_relaxed); }
bool rendering_firstperson()            { return g_rendering_fp.load(std::memory_order_relaxed); }

void note_firstperson_render_call()
{
    g_rendering_fp.store(true, std::memory_order_relaxed);
    c_render_calls.fetch_add(1, std::memory_order_relaxed);
    c_render_calls_frame.fetch_add(1, std::memory_order_relaxed);
}

bool armed()
{
    return g_wanted.load(std::memory_order_relaxed) && g_effect.load(std::memory_order_relaxed);
}

bool consider_fp_draw(unsigned int program, int count, bool is_entity,
                      bool has_normal, unsigned int tex0)
{
    c_window_draws_frame.fetch_add(1, std::memory_order_relaxed);
    const bool is3d = is_entity || has_normal;

    // The world's hand + held item come first (3D). The HUD's own 2D draws follow,
    // and only AFTER them come the hotbar/inventory item models (also 3D). So once
    // we've captured the item and the first 2D draw arrives, latch the window shut
    // — that is what stops the hotbar/inventory items from being captured (which is
    // what made them go invisible).
    if (!is3d) {
        if (t_captured_any) t_hud_started = true;
        return false;
    }
    if (t_hud_started) return false;

    // A 3D draw in the window, before the HUD: this is the first-person item/arm.
    g_item_prog.store((GLuint)program, std::memory_order_relaxed);
    g_item_tex.store((GLuint)tex0, std::memory_order_relaxed);
    g_item_count.store((GLsizei)count, std::memory_order_relaxed);
    c_item_draws.fetch_add(1, std::memory_order_relaxed);
    c_item_draws_frame.fetch_add(1, std::memory_order_relaxed);

    const bool arm = g_effect.load(std::memory_order_relaxed)
                  && (g_bloom_ready.load(std::memory_order_relaxed)
                      || g_prog_ready.load(std::memory_order_relaxed));
    if (arm) t_captured_any = true;

    if (g_debug.load(std::memory_order_relaxed))
        ITEMSHADER_RL(500, "FP-item draw: count=%d program=%u entity=%d normal=%d tex0=%u armed=%d",
                      count, program, (int)is_entity, (int)has_normal, tex0, (int)arm);
    return arm;
}

void run_capture(const std::function<void()>& redraw)
{
    if (g_bloom_ready.load(std::memory_order_relaxed)) capture(redraw);
    else                                               tap_outline(redraw);
}

void on_frame_boundary()
{
    if (g_wanted.load(std::memory_order_relaxed) && g_debug.load(std::memory_order_relaxed)) {
        ITEMSHADER_RL(1000,
            "state: wanted=%d effect=%d | frame: renderCalls=%llu windowDraws=%llu itemDraws=%llu "
            "| totals: renderCalls=%llu itemDraws=%llu | lastProg=%u lastTex=%u",
            (int)g_wanted.load(std::memory_order_relaxed), (int)g_effect.load(std::memory_order_relaxed),
            c_render_calls_frame.load(std::memory_order_relaxed),
            c_window_draws_frame.load(std::memory_order_relaxed),
            c_item_draws_frame.load(std::memory_order_relaxed),
            c_render_calls.load(std::memory_order_relaxed),
            c_item_draws.load(std::memory_order_relaxed),
            (unsigned)g_item_prog.load(std::memory_order_relaxed),
            (unsigned)g_item_tex.load(std::memory_order_relaxed));
    }

    // Roll per-frame state and CLOSE the window (re-raised by next frame's detour).
    c_render_calls_frame.store(0, std::memory_order_relaxed);
    c_item_draws_frame.store(0, std::memory_order_relaxed);
    c_window_draws_frame.store(0, std::memory_order_relaxed);
    g_rendering_fp.store(false, std::memory_order_relaxed);
    t_captured_any = false;
    t_hud_started  = false;

    if (!g_effect.load(std::memory_order_relaxed)) return;

    dump_mc_shader();
    build_capture_program();
    build_bloom();
    blur_and_composite();
}

void shutdown_gl() {
    set_wanted(false);
    set_effect(false);
    set_rendering_firstperson(false);
    suppress_scope suppress;
    delete g_glow_src; g_glow_src = nullptr;
    delete g_glow_a; g_glow_a = nullptr;
    delete g_glow_b; g_glow_b = nullptr;
    delete g_quad; g_quad = nullptr;
    delete g_blur; g_blur = nullptr;
    delete g_comp; g_comp = nullptr;
    auto& api = algl::api();
    if (g_capture_prog && api.DeleteProgram) api.DeleteProgram(g_capture_prog);
    g_capture_prog = 0;
    g_bloom_ready.store(false);
    g_prog_ready.store(false);
}

} // namespace vanta::modules::itemshader
