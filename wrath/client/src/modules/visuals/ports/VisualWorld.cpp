#include "PortJni.h"
#include "VisualPorts.h"
#include "modules/render/arraylist/al_gl.h"
#include "modules/render/chams/chams_gl.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "modules/render/outline/outline_view.h"
#include "modules/visuals/bloom/Bloom.h"
#include "modules/visuals/bloom/NativeTextures.h"
#include "modules/visuals/bloom/native/NativeImages.h"
#include "util/Log.h"
#include "vgui/bridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
namespace vanta::modules::visualports {
namespace {
namespace gl = ::vanta::al::gl;
jobject level = nullptr;
GLuint target = 0, vao = 0, vbo = 0, effectsAtlas = 0;
int width = 0, height = 0;
al::al_shader solidShader;
struct State {
    GLint draw, read, program, va, buffer, unpackBuffer, active, texture, sampler, viewport[4], src, dst, srcA, dstA,
        eq, eqA, depthfunc;
    GLboolean blend, depth, cull, scissor, stencil, mask[4], depthMask;
    using Blend = void(APIENTRY *)(GLenum, GLenum, GLenum, GLenum);
    using Eq = void(APIENTRY *)(GLenum, GLenum);
    Blend blendFunc;
    Eq blendEq;
    bool suppressed;
    State() {
        suppressed = chams::gl::exchange_suppressed(true);
        const auto &g = gl::api();
        glGetIntegerv(0x8CA6, &draw);
        glGetIntegerv(0x8CAA, &read);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(0x85B5, &va);
        glGetIntegerv(0x8894, &buffer);
        glGetIntegerv(0x88EF, &unpackBuffer);
        g.BindBuffer(0x88EC, 0);
        glGetIntegerv(0x84E0, &active);
        g.ActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(0x8919, &sampler);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(0x80C9, &src);
        glGetIntegerv(0x80C8, &dst);
        glGetIntegerv(0x80CB, &srcA);
        glGetIntegerv(0x80CA, &dstA);
        glGetIntegerv(0x8009, &eq);
        glGetIntegerv(0x883D, &eqA);
        glGetIntegerv(GL_DEPTH_FUNC, &depthfunc);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        blendFunc = reinterpret_cast<Blend>(wglGetProcAddress("glBlendFuncSeparate"));
        blendEq = reinterpret_cast<Eq>(wglGetProcAddress("glBlendEquationSeparate"));
    }
    ~State() {
        const auto &g = gl::api();
        g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        g.BindFramebuffer(GL_READ_FRAMEBUFFER, read);
        g.UseProgram(program);
        g.BindVertexArray(va);
        g.BindBuffer(GL_ARRAY_BUFFER, buffer);
        g.BindBuffer(0x88EC, unpackBuffer);
        g.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        g.BindSampler(0, sampler);
        g.ActiveTexture(active);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        auto set = [](GLenum n, bool b) {
            if (b)
                glEnable(n);
            else
                glDisable(n);
        };
        set(GL_BLEND, blend);
        set(GL_DEPTH_TEST, depth);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_STENCIL_TEST, stencil);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glDepthMask(depthMask);
        glDepthFunc(depthfunc);
        if (blendFunc)
            blendFunc(src, dst, srcA, dstA);
        if (blendEq)
            blendEq(eq, eqA);
        chams::gl::set_suppressed(suppressed);
    }
};
bool programs() {
    if (solidShader.linked())
        return true;
    const char *vs = "#version 330 core\nlayout(location=0)in vec4 p;layout(location=1)in vec4 c;layout(location=2)in "
                     "vec3 sprite;out vec4 color;out vec3 shape;void main(){gl_Position=p;color=c;shape=sprite;}";
    const char *fs = R"GLSL(#version 330 core
uniform sampler2D effectsAtlas;in vec4 color;in vec3 shape;out vec4 outColor;
void main(){vec4 tex=shape.z>.5?texture(effectsAtlas,shape.xy):vec4(1.);outColor=color*tex;if(outColor.a<.001)discard;}
)GLSL";
    return solidShader.load(vs, fs, "Native visual effects");
}
struct Vertex {
    float x, y, z, w, r, g, b, a, u = 0, v = 0, kind = 0;
};
std::vector<Vertex> triangles;
hitfx::render_frame camera;
float mvp[16];
Vertex vertex(double x, double y, double z, RGBA c) {
    float p[4] = {static_cast<float>(x - camera.cam_x), static_cast<float>(y - camera.cam_y),
                  static_cast<float>(z - camera.cam_z), 1};
    Vertex v{};
    auto *o = &v.x;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            o[r] += mvp[k * 4 + r] * p[k];
    v.r = c.r;
    v.g = c.g;
    v.b = c.b;
    v.a = c.a;
    return v;
}
using P = std::array<double, 3>;
void quad(P a, P b, P c, P d, RGBA color) {
    auto av = vertex(a[0], a[1], a[2], color), bv = vertex(b[0], b[1], b[2], color),
         cv = vertex(c[0], c[1], c[2], color), dv = vertex(d[0], d[1], d[2], color);
    triangles.insert(triangles.end(), {av, bv, cv, av, cv, dv});
}
void atlasUv(Vertex &v, int kind, float u, float y) {
    kind = std::clamp(kind, 0, 26);
    v.u = bloom::facet::U0[kind] + (bloom::facet::U1[kind] - bloom::facet::U0[kind]) * u;
    v.v = bloom::facet::V0[kind] + (bloom::facet::V1[kind] - bloom::facet::V0[kind]) * y;
    v.kind = 1;
}
void worldMesh(const std::array<P, 4> &p, const std::array<RGBA, 4> &colors,
               const std::array<std::array<float, 2>, 4> &uv, float kind) {
    Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        v[i] = vertex(p[i][0], p[i][1], p[i][2], colors[i]);
        atlasUv(v[i], static_cast<int>(kind), uv[i][0], uv[i][1]);
    }
    triangles.insert(triangles.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}
void worldLine(P a, P b, float wa, float wb, RGBA ca, RGBA cb, int kind) {
    double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]},
           v[3] = {(a[0] + b[0]) * .5 - camera.cam_x, (a[1] + b[1]) * .5 - camera.cam_y,
                   (a[2] + b[2]) * .5 - camera.cam_z};
    double n[3] = {d[1] * v[2] - d[2] * v[1], d[2] * v[0] - d[0] * v[2], d[0] * v[1] - d[1] * v[0]},
           l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-9)
        return;
    for (auto &x : n)
        x /= l;
    P ac = a, ad = a, bc = b, bd = b;
    for (int i = 0; i < 3; ++i) {
        ac[i] += n[i] * wa;
        ad[i] -= n[i] * wa;
        bc[i] += n[i] * wb;
        bd[i] -= n[i] * wb;
    }
    auto av = vertex(ac[0], ac[1], ac[2], ca), bv = vertex(bc[0], bc[1], bc[2], cb),
         cv = vertex(bd[0], bd[1], bd[2], cb), dv = vertex(ad[0], ad[1], ad[2], ca);
    atlasUv(av, kind, 0, 0);
    atlasUv(bv, kind, 1, 0);
    atlasUv(cv, kind, 1, 1);
    atlasUv(dv, kind, 0, 1);
    triangles.insert(triangles.end(), {av, bv, cv, av, cv, dv});
}
void worldSprite(P center, float w, float h, float angle, int kind, RGBA c) {
    float cs = std::cos(angle), sn = std::sin(angle);
    Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        float x = (i == 0 || i == 3 ? -1.f : 1.f) * w, y = (i < 2 ? -1.f : 1.f) * h, rx = x * cs - y * sn,
              ry = x * sn + y * cs;
        P p = center;
        for (int k = 0; k < 3; ++k)
            p[k] += camera.modelview[k * 4] * rx + camera.modelview[k * 4 + 1] * ry;
        v[i] = vertex(p[0], p[1], p[2], c);
        atlasUv(v[i], kind, (i == 0 || i == 3) ? 0.f : 1.f, i < 2 ? 0.f : 1.f);
        if (kind == 23)
            v[i].z = -v[i].w * .9999f;
    }
    triangles.insert(triangles.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}
} // namespace
void afterWorld(JNIEnv *e) {
    if (e->PushLocalFrame(256) < 0)
        return;
    try {
        auto client = jni::mc(e), w = jni::world(e, client), me = jni::player(e, client);
        bool changed = !e->IsSameObject(w, level);
        if (changed) {
            if (level)
                e->DeleteGlobalRef(level);
            level = w ? e->NewGlobalRef(w) : nullptr;
            bloom::resetWorld();
        }
        if (!w || !me || !bloom::needsWorld()) {
            e->PopLocalFrame(nullptr);
            return;
        }
        const auto &g = gl::api();
        if (!g.ok) {
            e->PopLocalFrame(nullptr);
            return;
        }
        State state;
        auto fc = jni::cls(e, "net/minecraft/class_276");
        auto main = e->CallObjectMethod(client, jni::method(e, jni::cls(e, "net/minecraft/class_310"), "method_1522",
                                                            "()Lnet/minecraft/class_276;"));
        width = e->GetIntField(main, jni::field(e, fc, "field_1482", "I"));
        height = e->GetIntField(main, jni::field(e, fc, "field_1481", "I"));
        if (width <= 0 || height <= 0) {
            e->PopLocalFrame(nullptr);
            return;
        }
        auto getId = [&](const char *n) {
            auto t = e->CallObjectMethod(main, jni::method(e, fc, n, "()Lcom/mojang/blaze3d/textures/GpuTexture;"));
            return t ? e->CallIntMethod(t,
                                        jni::method(e, jni::cls(e, "net/minecraft/class_10868"), "method_68427", "()I"))
                     : 0;
        };
        auto color = getId("method_30277"), depth = getId("method_30278");
        jni::check(e);
        if (!color || !programs()) {
            e->PopLocalFrame(nullptr);
            return;
        }
        if (!target)
            g.GenFramebuffers(1, &target);
        g.BindFramebuffer(GL_FRAMEBUFFER, target);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00, GL_TEXTURE_2D, depth, 0);
        if (g.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            e->PopLocalFrame(nullptr);
            return;
        }
        if (!vao)
            g.GenVertexArrays(1, &vao);
        g.BindVertexArray(vao);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glEnable(GL_BLEND);
        if (state.blendFunc)
            state.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        if (state.blendEq)
            state.blendEq(0x8006, 0x8006);
        if (bloom::needsWorld()) {
            hitfx::capture::install();
            hitfx::capture::capture_now(e, true);
            if (hitfx::latest_frame(camera)) {
                camera.viewport_w = width;
                camera.viewport_h = height;
                float bob[16], mv[16];
                if (outline::view::ensure_cache(e) && outline::view::bob_matrix(e, bob)) {
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r) {
                            mv[c * 4 + r] = 0;
                            for (int k = 0; k < 4; ++k)
                                mv[c * 4 + r] += bob[k * 4 + r] * camera.modelview[c * 4 + k];
                        }
                    std::copy(mv, mv + 16, camera.modelview);
                }
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) {
                        mvp[c * 4 + r] = 0;
                        for (int k = 0; k < 4; ++k)
                            mvp[c * 4 + r] += camera.projection[k * 4 + r] * camera.modelview[c * 4 + k];
                    }
                g.BindVertexArray(vao);
                g.ActiveTexture(GL_TEXTURE0);
                g.BindSampler(0, 0);
                if (state.blendFunc)
                    state.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                triangles.clear();
                auto flush = [&](bool throughWalls) {
                    if (!triangles.empty()) {
                        if (throughWalls)
                            glDisable(GL_DEPTH_TEST);
                        else {
                            glEnable(GL_DEPTH_TEST);
                            glDepthFunc(GL_LEQUAL);
                        }
                        if (!vbo)
                            g.GenBuffers(1, &vbo);
                        g.BindBuffer(GL_ARRAY_BUFFER, vbo);
                        g.BufferData(GL_ARRAY_BUFFER, triangles.size() * sizeof(Vertex), triangles.data(), 0x88E0);
                        g.EnableVertexAttribArray(0);
                        g.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                        g.EnableVertexAttribArray(1);
                        g.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(16));
                        g.EnableVertexAttribArray(2);
                        g.VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(32));
                        auto &atlas = effectsAtlas;
                        if (!atlas)
                            atlas = bloom::loadNativeTexture(2302, false);
                        g.ActiveTexture(GL_TEXTURE0);
                        g.BindSampler(0, 0);
                        glBindTexture(GL_TEXTURE_2D, atlas);
                        g.BindVertexArray(vao);
                        solidShader.bind();
                        solidShader.set("effectsAtlas", 0);
                        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(triangles.size()));
                    }
                };
                triangles.clear();
                if (bloom::needsWorld()) {
                    bloom::Canvas canvas{quad, worldLine, worldSprite, worldMesh};
                    canvas.layer = [&](bool additive) {
                        flush(false);
                        triangles.clear();
                        if (state.blendFunc)
                            state.blendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                            GL_ONE_MINUS_SRC_ALPHA);
                    };
                    bloom::worldFrame(e, w, me, camera, canvas);
                    flush(false);
                    triangles.clear();
                    if (state.blendFunc)
                        state.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    triangles.clear();
                }
            }
        }
    } catch (const std::exception &ex) {
        (void)ex;
        if (e->ExceptionCheck())
            e->ExceptionClear();
        static bool logged = false;
        if (!logged) {
            logged = true;
            VLOG_WARN("visualports", "World effect: {}", ex.what());
        }
    }
    e->PopLocalFrame(nullptr);
}
void shutdownGl() {
    const auto &g = gl::api();
    if (g.ok) {
        State state;
        if (effectsAtlas)
            glDeleteTextures(1, &effectsAtlas);
        effectsAtlas = 0;
        solidShader.destroy();
        if (target)
            g.DeleteFramebuffers(1, &target);
        if (vao)
            g.DeleteVertexArrays(1, &vao);
        if (vbo)
            g.DeleteBuffers(1, &vbo);
    }
    target = vao = vbo = 0;
    jvm::JniScope s;
    if (s) {
        auto *e = s.get();
        if (level)
            e->DeleteGlobalRef(level);
        level = nullptr;
    }
}
} // namespace vanta::modules::visualports
