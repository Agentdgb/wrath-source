#include "SkyRenderer.h"
#include "AtmosphereMath.h"
#include "NativeTextures.h"
#include "modules/render/arraylist/al_gl.h"
#include "modules/visuals/ports/CharacterData.h"
#include <cstring>
namespace vanta::modules::bloom {
namespace {
namespace gl = al::gl;
al::al_shader shader;
GLuint vao = 0, vbo = 0;
std::array<GLuint, 7> textures{};
bool starsLoaded = false;
std::string error;
struct Vertex {
    float p[4], c[4], uv[2];
};
bool program() {
    if (shader.linked())
        return true;
    const char *vs = "#version 330 core\nlayout(location=0)in vec4 p;layout(location=1)in vec4 c;layout(location=2)in "
                     "vec2 uv;out vec4 color;out vec2 tex;void main(){gl_Position=p;color=c;tex=uv;}";
    const char *fs = "#version 330 core\nuniform sampler2D image;uniform int flatMode;in vec4 color;in vec2 tex;out "
                     "vec4 outColor;void main(){outColor=color*(flatMode!=0?vec4(1.):texture(image,tex));}";
    return shader.load(vs, fs, "Native Atmosphere");
}
void stream(const std::vector<Vertex> &data, bool flat) {
    if (data.empty())
        return;
    const auto &g = gl::api();
    if (!vao)
        g.GenVertexArrays(1, &vao);
    if (!vbo)
        g.GenBuffers(1, &vbo);
    g.BindVertexArray(vao);
    g.BindBuffer(GL_ARRAY_BUFFER, vbo);
    g.BufferData(GL_ARRAY_BUFFER, data.size() * sizeof(Vertex), data.data(), 0x88E0);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    g.EnableVertexAttribArray(1);
    g.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(16));
    g.EnableVertexAttribArray(2);
    g.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(32));
    shader.bind();
    shader.set("image", 0);
    shader.set("flatMode", flat ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(data.size()));
}
using Blend = void(APIENTRY *)(GLenum, GLenum, GLenum, GLenum);
Blend blend() { return reinterpret_cast<Blend>(wglGetProcAddress("glBlendFuncSeparate")); }
} // namespace
const char *atmosphereSkyError() { return error.empty() ? nullptr : error.c_str(); }
void drawAtmosphereSky(const hitfx::render_frame &frame) {
    auto &model = atmosphereModel();
    if (!model.overworld || model.weather.cv * model.weather.da < .002f) {
        sky::reset();
        return;
    }
    try {
        if (!program())
            throw std::runtime_error("Atmosphere shader unavailable");
        // Upload one original atlas per frame instead of stalling the first toggle.
        for (int i = 0; i < 7; ++i)
            if (!textures[i]) {
                textures[i] = loadNativeTexture(2400 + i, true, i >= 2 && i <= 5);
                return;
            }
        if (!starsLoaded) {
            auto raw = visualports::characterResource(2450);
            if (raw.size() != 96004 || std::memcmp(raw.data(), "WBS1", 4))
                throw std::runtime_error("Invalid star data");
            Array<float> positions(21600);
            Array<int> colors(2400);
            for (int i = 0; i < 21600; ++i)
                std::memcpy(&positions[i], raw.data() + 4 + i * 4, 4);
            for (int i = 0; i < 2400; ++i)
                std::memcpy(&colors[i], raw.data() + 86404 + i * 4, 4);
            sky::starData(positions, colors);
            starsLoaded = true;
        }
        sky::Input input;
        input.width = frame.viewport_w;
        input.height = frame.viewport_h;
        input.fov = 2 * std::atan(1 / std::max(.01f, frame.projection[5])) * 180 / Math::PI;
        input.dt = atmosphereFrameDelta();
        input.light = atmosphereNaturalLight();
        input.underwater = model.submerged;
        for (int k = 0; k < 3; ++k) {
            input.right[k] = frame.modelview[k * 4];
            input.up[k] = frame.modelview[k * 4 + 1];
            input.forward[k] = -frame.modelview[k * 4 + 2];
        }
        sky::Buffers batches;
        sky::generate(model.weather, input, batches);
        float matrix[16]{};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k)
                    matrix[col * 4 + row] += frame.projection[k * 4 + row] * frame.modelview[col * 4 + k];
        const auto &g = gl::api();
        g.ActiveTexture(GL_TEXTURE0);
        g.BindSampler(0, 0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        static constexpr int imageIndex[] = {0, 1, 1, 2, 6, 3, 4, 5};
        auto setBlend = blend();
        for (auto &batch : batches.streams) {
            std::vector<Vertex> vertices;
            vertices.reserve(batch.vertices.size() / 4 * 6);
            for (std::size_t i = 0; i + 3 < batch.vertices.size(); i += 4) {
                Vertex quad[4]{};
                for (int n = 0; n < 4; ++n) {
                    auto &v = batch.vertices[i + n];
                    float p[] = {v.x, v.y, v.z, 1};
                    for (int row = 0; row < 4; ++row)
                        for (int k = 0; k < 4; ++k)
                            quad[n].p[row] += matrix[k * 4 + row] * p[k];
                    quad[n].p[2] = quad[n].p[3] * .999999f;
                    for (int k = 0; k < 3; ++k)
                        quad[n].c[k] = float(v.color >> (16 - 8 * k) & 255) / 255;
                    quad[n].c[3] = float(v.color >> 24) / 255;
                    quad[n].uv[0] = v.u;
                    quad[n].uv[1] = v.v;
                }
                vertices.insert(vertices.end(), {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]});
            }
            glBindTexture(GL_TEXTURE_2D, textures[imageIndex[batch.layer]]);
            bool additive = batch.layer == 2 || batch.layer == 3 || batch.layer == 6 || batch.layer == 7;
            bool premult = batch.layer == 1 || batch.layer == 4;
            if (setBlend)
                setBlend(premult ? GL_ONE : GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                         GL_ONE_MINUS_SRC_ALPHA);
            stream(vertices, false);
        }
        error.clear();
    } catch (const std::exception &ex) {
        error = ex.what();
        sky::reset();
    }
}
void drawAtmosphereTint() {
    auto &m = atmosphereModel();
    float amount = std::clamp(m.fade * m.tint, 0.f, 1.f) * (1 - std::clamp(m.weather.cv * m.weather.da, 0.f, 1.f));
    if (amount < .001f)
        return;
    try {
        if (!program())
            throw std::runtime_error("Atmosphere tint shader unavailable");
        auto b = m.base;
        float maximum = std::max({b[0], b[1], b[2]});
        if (maximum <= 0)
            return;
        auto pass = [&](bool add) {
            std::vector<Vertex> v(3);
            float p[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
            for (int n = 0; n < 3; ++n) {
                v[n].p[0] = p[n][0];
                v[n].p[1] = p[n][1];
                v[n].p[3] = 1;
                for (int k = 0; k < 3; ++k)
                    v[n].c[k] = add ? b[k] * .1f * amount : 1 - amount * (1 - b[k] / maximum);
                v[n].c[3] = 1;
            }
            auto setBlend = blend();
            if (setBlend)
                setBlend(add ? GL_ONE : GL_DST_COLOR, add ? GL_ONE : GL_ZERO, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            stream(v, true);
        };
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        pass(false);
        pass(true);
    } catch (const std::exception &ex) {
        error = ex.what();
    }
}
void shutdownAtmosphereGl() {
    shader.destroy();
    const auto &g = gl::api();
    if (g.ok) {
        if (vao)
            g.DeleteVertexArrays(1, &vao);
        if (vbo)
            g.DeleteBuffers(1, &vbo);
    }
    for (auto &t : textures) {
        if (t)
            glDeleteTextures(1, &t);
        t = 0;
    }
    vao = vbo = 0;
    starsLoaded = false;
    sky::starData({}, {});
    sky::reset();
}
} // namespace vanta::modules::bloom
