#pragma once
#include "../BloomCompat.h"
#include <deque>
namespace dt::modules::bloom::sky {
struct Rotation {
    std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
};
struct SkyVertex {
    float x = 0, y = 0, z = 0, u = 0, v = 0;
    std::uint32_t color = 0xFFFFFFFF;
};
struct Stream {
    int layer = 0;
    std::vector<SkyVertex> vertices;
    Stream &vertex(float x, float y, float z) {
        if (vertices.size() >= 250000)
            throw std::runtime_error("Sky vertex limit");
        vertices.push_back({x, y, z});
        return *this;
    }
    Stream &vertex(const Rotation &r, float x, float y, float z) {
        return vertex(r.m[0] * x + r.m[3] * y + r.m[6] * z, r.m[1] * x + r.m[4] * y + r.m[7] * z,
                      r.m[2] * x + r.m[5] * y + r.m[8] * z);
    }
    Stream &texture(float u, float v) {
        vertices.back().u = u;
        vertices.back().v = v;
        return *this;
    }
    Stream &color(std::uint32_t value) {
        vertices.back().color = value;
        return *this;
    }
    Stream &color(int r, int g, int b, int a) {
        return color(std::uint32_t(std::clamp(a, 0, 255)) << 24 | std::uint32_t(std::clamp(r, 0, 255)) << 16 |
                     std::uint32_t(std::clamp(g, 0, 255)) << 8 | std::uint32_t(std::clamp(b, 0, 255)));
    }
};
struct Buffers {
    std::deque<Stream> streams;
    Stream *getBuffer(int layer) {
        if (streams.empty() || streams.back().layer != layer)
            streams.push_back({layer});
        return &streams.back();
    }
};
struct State {
    Array<float> pw = Array<float>(13), cs = Array<float>(31);
    float cv = 0, da = .9f, db = .5f, dc = .7f, di = .45f, dj = 0, er = 0, es = 180, eu = 1, ep = 0, eq = 1, dk = 1;
    double dd = 0;
    bool stars = true;
    bool o() const { return stars; }
    float x() const { return ep * std::min(1.f, eq * 1.6f) * dk; }
    float w() const { return es; }
};
struct Input {
    int width = 1920, height = 1080;
    float fov = 90, dt = 1.f / 60, light = 1;
    bool underwater = false;
    std::array<float, 3> right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, -1};
};
void generate(State &, const Input &, Buffers &);
void profile(Array<float> &out, int index, float density);
void reset();
float moonCoverage();
float sunCoverage();
void starData(Array<float> positions, Array<int> colors);
} // namespace dt::modules::bloom::sky
