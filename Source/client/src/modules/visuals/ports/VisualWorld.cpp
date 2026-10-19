#include "PortJni.h"
#include "VisualPorts.h"
#include "modules/render/arraylist/al_gl.h"
#include "modules/render/chams/chams_gl.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "modules/render/outline/outline_view.h"
#include "modules/visuals/bloom/Bloom.h"
#include "modules/visuals/bloom/DeathEffects.h"
#include "modules/visuals/bloom/NativeTextures.h"
#include "modules/visuals/bloom/SkyRenderer.h"
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
using Clock = std::chrono::steady_clock;
struct Spirit {
    double x, y, z;
    float bodyYaw, headYaw, headPitch, walk, stride, scale;
    bool crouch, mine;
    Clock::time_point born;
};
std::deque<Spirit> spirits;
std::mutex packetMutex;
struct PendingPop {
    jobject packet, world;
};
std::deque<PendingPop> pending;
jclass statusPacket = nullptr, bundlePacket = nullptr;
jmethodID statusCode = nullptr;
jobject level = nullptr;
GLuint target = 0, history = 0, vao = 0, vbo = 0, effectsAtlas = 0;
int width = 0, height = 0;
bool primed = false;
Clock::time_point last{};
int seed = 0;
al::al_shader blurShader, solidShader;
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
    if (solidShader.linked() && blurShader.linked())
        return true;
    const char *vs = "#version 330 core\nlayout(location=0)in vec4 p;layout(location=1)in vec4 c;layout(location=2)in "
                     "vec3 sprite;out vec4 color;out vec3 shape;void main(){gl_Position=p;color=c;shape=sprite;}";
    const char *fs = R"GLSL(#version 330 core
uniform sampler2D effectsAtlas;in vec4 color;in vec3 shape;out vec4 outColor;
void main(){vec4 tex=shape.z>.5?texture(effectsAtlas,shape.xy):vec4(1.);outColor=color*tex;if(outColor.a<.001)discard;}
)GLSL";
    const char *bvs = "#version 330 core\nout vec2 uv;void main(){vec2 "
                      "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.-1.,0,1);}";
    const char *bfs = "#version 330 core\nuniform sampler2D history;uniform float retention;uniform float seed;in vec2 "
                      "uv;out vec4 outColor;void main(){float "
                      "n=fract(52.9829189*fract(dot(gl_FragCoord.xy+seed,vec2(.06711056,.00583715))));outColor=vec4("
                      "texture(history,uv).rgb+(n-.5)/(255.*retention),retention);}";
    return solidShader.load(vs, fs, "TotemSpirit") && blurShader.load(bvs, bfs, "MotionBlur");
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
void line(P a, P b, RGBA color, float pixels) {
    auto av = vertex(a[0], a[1], a[2], color), bv = vertex(b[0], b[1], b[2], color);
    if (av.w < .05f || bv.w < .05f)
        return;
    float dx = (bv.x / bv.w - av.x / av.w) * width, dy = (bv.y / bv.w - av.y / av.w) * height, l = std::hypot(dx, dy);
    if (l < .01f)
        return;
    float x = -dy / l * pixels / width, y = dx / l * pixels / height;
    auto cv = bv, dv = av;
    av.x += x * av.w;
    av.y += y * av.w;
    bv.x += x * bv.w;
    bv.y += y * bv.w;
    cv.x -= x * cv.w;
    cv.y -= y * cv.w;
    dv.x -= x * dv.w;
    dv.y -= y * dv.w;
    triangles.insert(triangles.end(), {av, bv, cv, av, cv, dv});
}
RGBA alpha(RGBA c, float a) {
    c.a = a;
    return c;
}
RGBA mix(RGBA a, RGBA b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1}; }
void ring(const Spirit &s, double y, float inner, float outer, RGBA color) {
    for (int i = 0; i < 40; ++i) {
        float a = 2 * math::pi * i / 40, b = 2 * math::pi * (i + 1) / 40;
        quad({s.x + std::cos(a) * inner, y, s.z + std::sin(a) * inner},
             {s.x + std::cos(b) * inner, y, s.z + std::sin(b) * inner},
             {s.x + std::cos(b) * outer, y, s.z + std::sin(b) * outer},
             {s.x + std::cos(a) * outer, y, s.z + std::sin(a) * outer}, color);
    }
}
void ankh(P origin, float size, RGBA color, bool flat, float yaw = 0) {
    auto point = [&](float x, float y) -> P {
        if (flat)
            return {origin[0] + (x * std::cos(yaw) - y * std::sin(yaw)) * size, origin[1],
                    origin[2] + (x * std::sin(yaw) + y * std::cos(yaw)) * size};
        return {origin[0] + (camera.modelview[0] * x + camera.modelview[1] * y) * size,
                origin[1] + (camera.modelview[4] * x + camera.modelview[5] * y) * size,
                origin[2] + (camera.modelview[8] * x + camera.modelview[9] * y) * size};
    };
    for (int i = 0; i < 24; ++i) {
        float a = 2 * math::pi * i / 24, b = 2 * math::pi * (i + 1) / 24;
        line(point(std::cos(a) * .5f, .5f + std::sin(a) * .6f), point(std::cos(b) * .5f, .5f + std::sin(b) * .6f),
             color, 1.5f);
    }
    line(point(0, -1), point(0, .05f), color, 2);
    line(point(-.65f, -.25f), point(.65f, -.25f), color, 2);
}
void drawSpirit(const Spirit &s, float t) {
    float a = spirit.opacity * std::min(1.f, t / .06f), k = std::clamp((t - .08f) / .45f, 0.f, 1.f);
    k = spirit.pose ? k * k * (3 - 2 * k) : 0;
    float lift = spirit.ascent * math::ascend(t), unit = .05859375f * s.scale * (1 + .08f * t);
    auto pose = math::spiritPose(s.walk, s.stride, s.headPitch, s.headYaw, s.crouch, k);
    RGBA gold = spirit.body, lapis = spirit.aura;
    if (!spirit.manual) {
        vgui::bridge::clientAccent(gold.r, gold.g, gold.b, gold.a);
        lapis = {gold.r * .45f, gold.g * .45f, gold.b * .75f, 1};
    }
    auto light = mix(gold, {1, 1, 1, 1}, .55f);
    static const int faces[24] = {1, 3, 7, 5, 0, 4, 6, 2, 2, 6, 7, 3, 0, 1, 5, 4, 4, 5, 7, 6, 0, 2, 3, 1},
                     edges[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};
    static const float tiers[] = {0, 0, .4f, .6f, .6f, 1};
    for (int shell = 1; shell >= 0; --shell)
        for (int i = 0; i < 6; ++i) {
            float fade = a * math::dissolve(t, tiers[i]);
            if (fade < .01f)
                continue;
            std::array<std::array<float, 3>, 8> v;
            math::spiritCorners(i, shell ? .7f : 0, pose, s.crouch, k, unit, s.bodyYaw, v);
            P p[8];
            for (int n = 0; n < 8; ++n)
                p[n] = {s.x + v[n][0], s.y + lift + v[n][1], s.z + v[n][2]};
            if (shell || spirit.look == 0) {
                auto c = shell ? alpha(lapis, fade * (spirit.look == 0 ? .16f : .1f))
                               : alpha(mix(gold, lapis, (1 - tiers[i]) * .45f), fade * .32f);
                for (int f = 0; f < 24; f += 4)
                    quad(p[faces[f]], p[faces[f + 1]], p[faces[f + 2]], p[faces[f + 3]], c);
            }
            if (!shell)
                for (int e = 0; e < 24; e += 2)
                    line(p[edges[e]], p[edges[e + 1]], alpha(light, fade * (spirit.look == 0 ? .8f : .95f)),
                         spirit.look == 0 ? 1.5f : 2);
        }
    if (spirit.seal && t < .6f) {
        float st = t / .6f, open = 1 - std::pow(1 - st, 3.f), r = (.35f + 1.15f * open) * s.scale,
              fade = a * (1 - st) * .9f;
        ring(s, s.y + .02, r * .93f, r, alpha(gold, fade));
        ring(s, s.y + .02, r, r * 1.12f, alpha(gold, fade * .25f));
        ring(s, s.y + .02, r * .55f, r * .6f, alpha(lapis, fade * .7f));
        for (int i = 0; i < 4; ++i) {
            float angle = s.bodyYaw * math::deg + open * .6f + i * math::pi / 2;
            ankh({s.x + std::cos(angle) * r * .78f, s.y + .024, s.z + std::sin(angle) * r * .78f}, .13f * s.scale,
                 alpha(light, fade), true, -angle - math::pi / 2);
        }
    }
    if (spirit.crest) {
        float pop = std::min(1.f, t / .2f), u = pop - 1,
              size = .2f * s.scale * std::max(0.f, 1 + 1.7f * u * u * u + .7f * u * u),
              fade = a * math::dissolve(t, 1.1f);
        ankh({s.x, s.y + lift + (32 - (s.crouch ? 3 * (1 - k) : 0)) * unit + .35f * s.scale + lift * .3f, s.z}, size,
             alpha(light, fade), false);
    }
}
void capture(JNIEnv *e, jobject event, jobject w, jobject me) {
    auto c = jni::cls(e, "net/minecraft/class_2663");
    if (!e->IsInstanceOf(event, c))
        return;
    auto status = e->CallByteMethod(event, jni::method(e, c, "method_11470", "()B"));
    if (status == 3) {
        bloom::deathPacket(e, event, w, me);
        return;
    }
    if (status != 35 || !spirit.enabled)
        return;
    auto entity = e->CallObjectMethod(
        event, jni::method(e, c, "method_11469", "(Lnet/minecraft/class_1937;)Lnet/minecraft/class_1297;"), w);
    jni::check(e);
    if (!entity || !e->IsInstanceOf(entity, jni::cls(e, "net/minecraft/class_1657")))
        return;
    bool own = e->IsSameObject(entity, me);
    if (own && !spirit.own)
        return;
    auto ec = jni::cls(e, "net/minecraft/class_1297"), lc = jni::cls(e, "net/minecraft/class_1309");
    auto d = [&](const char *n) { return e->CallDoubleMethod(entity, jni::method(e, ec, n, "()D")); };
    Spirit s{};
    s.x = d("method_23317");
    s.y = d("method_23318");
    s.z = d("method_23321");
    s.bodyYaw = e->GetFloatField(entity, jni::field(e, lc, "field_6283", "F"));
    s.headYaw = std::remainder(e->GetFloatField(entity, jni::field(e, lc, "field_6241", "F")) - s.bodyYaw, 360.f);
    s.headPitch = e->CallFloatMethod(entity, jni::method(e, ec, "method_36455", "()F"));
    s.scale = 1;
    auto scale = jni::optional(e, lc, "method_55693", "()F");
    if (scale)
        s.scale = e->CallFloatMethod(entity, scale);
    s.crouch = e->CallBooleanMethod(entity, jni::method(e, ec, "method_18276", "()Z"));
    auto anim = e->GetObjectField(entity, jni::field(e, lc, "field_42108", "Lnet/minecraft/class_8080;"));
    if (anim) {
        auto ac = jni::cls(e, "net/minecraft/class_8080");
        s.walk = e->CallFloatMethod(anim, jni::method(e, ac, "method_48569", "()F"));
        s.stride = std::min(1.f, e->CallFloatMethod(anim, jni::method(e, ac, "method_48566", "()F")));
    }
    jni::check(e);
    s.mine = own;
    s.born = Clock::now();
    if (spirits.size() == 12)
        spirits.pop_front();
    spirits.push_back(s);
}
void processPackets(JNIEnv *e, jobject w, jobject me) {
    std::deque<PendingPop> q;
    {
        std::lock_guard l(packetMutex);
        q.swap(pending);
    }
    for (auto entry : q) {
        auto packet = entry.packet;
        if (e->PushLocalFrame(128) >= 0) {
            try {
                if ((spirit.enabled || bloom::deathInterested()) && w && e->IsSameObject(w, entry.world)) {
                    auto bundle = jni::cls(e, "net/minecraft/class_8042");
                    if (e->IsInstanceOf(packet, bundle)) {
                        auto iterable = e->CallObjectMethod(
                            packet, jni::method(e, bundle, "method_48324", "()Ljava/lang/Iterable;"));
                        auto it = e->CallObjectMethod(iterable, jni::method(e, jni::cls(e, "java/lang/Iterable"),
                                                                            "iterator", "()Ljava/util/Iterator;"));
                        auto ic = jni::cls(e, "java/util/Iterator");
                        auto has = jni::method(e, ic, "hasNext", "()Z"),
                             next = jni::method(e, ic, "next", "()Ljava/lang/Object;");
                        while (e->CallBooleanMethod(it, has)) {
                            auto p = e->CallObjectMethod(it, next);
                            capture(e, p, w, me);
                            e->DeleteLocalRef(p);
                        }
                    } else
                        capture(e, packet, w, me);
                }
            } catch (...) {
                if (e->ExceptionCheck())
                    e->ExceptionClear();
            }
            e->PopLocalFrame(nullptr);
        }
        e->DeleteGlobalRef(packet);
        if (entry.world)
            e->DeleteGlobalRef(entry.world);
    }
}
} // namespace
void receive(JNIEnv *e, jobject packet) {
    if (isStopping())
        return;
    if (e->PushLocalFrame(32) < 0)
        return;
    struct Frame {
        JNIEnv *env;
        ~Frame() { env->PopLocalFrame(nullptr); }
    } frame{e};
    std::lock_guard l(packetMutex);
    try {
        if (!statusPacket) {
            auto c = jni::cls(e, "net/minecraft/class_2663");
            statusCode = jni::method(e, c, "method_11470", "()B");
            statusPacket = static_cast<jclass>(e->NewGlobalRef(c));
            e->DeleteLocalRef(c);
            auto b = jni::cls(e, "net/minecraft/class_8042");
            bundlePacket = static_cast<jclass>(e->NewGlobalRef(b));
            e->DeleteLocalRef(b);
        }
        int code = e->IsInstanceOf(packet, statusPacket) ? e->CallByteMethod(packet, statusCode) : -1;
        bool status = code == 35 || (code == 3 && bloom::deathInterested());
        if ((status || (bundlePacket && e->IsInstanceOf(packet, bundlePacket))) && pending.size() < 64) {
            auto world = jni::world(e, jni::mc(e));
            auto worldRef = world ? e->NewGlobalRef(world) : nullptr;
            auto ref = e->NewGlobalRef(packet);
            if (ref)
                pending.push_back({ref, worldRef});
            else if (worldRef)
                e->DeleteGlobalRef(worldRef);
        }
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
    }
}
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
            spirits.clear();
            bloom::resetWorld();
            primed = false;
        }
        processPackets(e, w, me);
        if (!spirit.enabled)
            spirits.clear();
        if (!w || !me || (!blurEnabled && spirits.empty() && !bloom::needsWorld())) {
            primed = false;
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
            primed = false;
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
            primed = false;
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
        if (!spirits.empty() || bloom::needsWorld()) {
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
                bloom::drawAtmosphereSky(camera);
                g.BindVertexArray(vao);
                g.ActiveTexture(GL_TEXTURE0);
                g.BindSampler(0, 0);
                if (state.blendFunc)
                    state.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                triangles.clear();
                auto now = Clock::now();
                while (!spirits.empty() &&
                       std::chrono::duration<float>(now - spirits.front().born).count() >= spirit.life)
                    spirits.pop_front();
                for (auto &s : spirits) {
                    if (s.mine && !spirit.own)
                        continue;
                    double dx = s.x - camera.cam_x, dy = s.y - camera.cam_y, dz = s.z - camera.cam_z;
                    if (dx * dx + dy * dy + dz * dz <= 9216)
                        drawSpirit(s, std::min(1.f, std::chrono::duration<float>(now - s.born).count() / spirit.life));
                }
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
                        if (!effectsAtlas)
                            effectsAtlas = bloom::loadNativeTexture(2302);
                        g.ActiveTexture(GL_TEXTURE0);
                        g.BindSampler(0, 0);
                        glBindTexture(GL_TEXTURE_2D, effectsAtlas);
                        g.BindVertexArray(vao);
                        solidShader.bind();
                        solidShader.set("effectsAtlas", 0);
                        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(triangles.size()));
                    }
                };
                flush(spirit.walls);
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
                }
                bloom::drawAtmosphereTint();
            }
        }
        if (blurEnabled) {
            g.BindVertexArray(vao);
            if (state.blendFunc)
                state.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            auto focused = jni::optional(e, jni::cls(e, "net/minecraft/class_310"), "method_53466", "()Z");
            bool active = !focused || e->CallBooleanMethod(client, focused);
            auto now = Clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            g.ActiveTexture(GL_TEXTURE0);
            g.BindSampler(0, 0);
            if (!history) {
                glGenTextures(1, &history);
                primed = false;
            }
            glBindTexture(GL_TEXTURE_2D, history);
            GLint tw = 0, th = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            if (tw != width || th != height) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                primed = false;
            }
            float r = math::retention(blurStrength, dt);
            if (primed && active && r >= 1.f / 255) {
                glDisable(GL_DEPTH_TEST);
                blurShader.bind();
                blurShader.set("history", 0);
                blurShader.set("retention", r);
                blurShader.set("seed", static_cast<float>((++seed & 63) * 7));
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
            primed = active;
        } else
            primed = false;
    } catch (const std::exception &ex) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        primed = false;
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
        bloom::shutdownAtmosphereGl();
        if (effectsAtlas)
            glDeleteTextures(1, &effectsAtlas);
        effectsAtlas = 0;
        blurShader.destroy();
        solidShader.destroy();
        if (target)
            g.DeleteFramebuffers(1, &target);
        if (vao)
            g.DeleteVertexArrays(1, &vao);
        if (vbo)
            g.DeleteBuffers(1, &vbo);
        if (history)
            glDeleteTextures(1, &history);
    }
    target = history = vao = vbo = 0;
    primed = false;
    jvm::JniScope s;
    if (s) {
        auto *e = s.get();
        if (level)
            e->DeleteGlobalRef(level);
        level = nullptr;
        std::lock_guard l(packetMutex);
        for (auto p : pending) {
            e->DeleteGlobalRef(p.packet);
            if (p.world)
                e->DeleteGlobalRef(p.world);
        }
        pending.clear();
        if (statusPacket)
            e->DeleteGlobalRef(statusPacket);
        if (bundlePacket)
            e->DeleteGlobalRef(bundlePacket);
        statusPacket = bundlePacket = nullptr;
    }
    spirits.clear();
}
} // namespace vanta::modules::visualports
