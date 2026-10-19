#include "VisualPorts.h"
#include <algorithm>
#include <cmath>
namespace vanta::modules::visualports::math {
float crest(float t, float peak) {
    if (t <= 0 || t >= 1)
        return 0;
    if (t < peak) {
        float u = 1 - t / peak;
        return 1 - u * u * u;
    }
    float u = (t - peak) / (1 - peak);
    return 1 - u * u * (3 - 2 * u);
}
float retention(float s, double dt) {
    return dt > 0 && dt < 1 ? static_cast<float>(std::pow(std::clamp(s, .1f, .9f), dt * 60)) : 0;
}
std::vector<Op> swingPose(int style, float t, int side, float k) {
    std::vector<Op> o;
    if (t <= 0 || t >= 1)
        return o;
    auto tr = [&](float x, float y, float z) { o.push_back({-1, x, y, z}); };
    auto rx = [&](float a) { o.push_back({0, a, 0, 0}); };
    auto ry = [&](float a) { o.push_back({1, 0, a, 0}); };
    auto rz = [&](float a) { o.push_back({2, 0, 0, a}); };
    switch (style) {
    case 1: {
        float raise = t < .28f ? std::sin(pi * t / .28f) * k : 0,
              strike = t <= .18f ? 0 : crest((t - .18f) / .82f, .28f) * k;
        tr(side * -.06f * strike, .16f * raise - .1f * strike, .06f * raise - .24f * strike);
        rx(30 * raise - 105 * strike);
        rz(side * 10 * strike);
        break;
    }
    case 2: {
        float a = crest(t, .42f), hook = a * a * k;
        a *= k;
        tr(side * -.1f * a, -.24f * a, -.06f * a);
        rz(side * -100 * a);
        ry(side * 30 * hook);
        rx(-25 * a);
        break;
    }
    case 3: {
        float turn = 2 * pi * t, c = std::cos(turn), s = std::sin(turn), body = std::sin(pi * t) * k, r = .17f * k;
        tr(side * r * .6f * (c - 1), r * s, -.12f * body);
        rz(side * 40 * s * k);
        rx(-40 * body);
        break;
    }
    case 4: {
        float beat = std::sin(2 * pi * t), w = beat * beat * (t < .5f ? 1 : .65f) * k;
        tr(side * -.08f * w, .06f * w, -.1f * w);
        ry(side * 26 * w);
        rx(-50 * w);
        rz(side * -22 * w);
        break;
    }
    case 5: {
        float a = (.5f - .5f * std::cos(2 * pi * t)) * k;
        tr(0, -.06f * a, -.1f * a);
        rx(-24 * a);
        ry(side * 10 * a);
        break;
    }
    default: {
        float a = crest(t, .3f) * k;
        tr(side * -.3f * a, .05f * std::sin(pi * t) * k, -.1f * a);
        ry(side * 64 * a);
        rz(side * 48 * a);
        rx(-14 * a);
        break;
    }
    }
    return o;
}
float ascend(float t) {
    float k = std::max(0.f, (t - .12f) / .88f);
    return k * k * (2.2f - 1.2f * k);
}
float dissolve(float t, float tier) {
    return 1 - std::clamp((t - .45f - tier * .22f) / .3f, 0.f, 1.f);
}
SpiritPose spiritPose(float walk, float stride, float hp, float hy, bool crouch, float ka) {
    SpiritPose p;
    float w = std::cos(walk * .6662f) * stride, s = 1 - ka, c = crouch ? .35f : 0;
    p.pitch[0] = w * 1.4f * s - .05f * ka;
    p.pitch[1] = -w * 1.4f * s - .15f * ka;
    p.pitch[3] = (-w + c) * s + 2.75f * ka;
    p.pitch[4] = (w + c) * s + 2.75f * ka;
    p.pitch[5] = -hp * deg * s + .3f * ka;
    p.yaw[5] = hy * deg * s;
    p.roll[3] = .32f * ka;
    p.roll[4] = -.32f * ka;
    return p;
}
void spiritCorners(int i, float inflate, const SpiritPose &p, bool crouch, float ka, float unit, float yaw,
                   std::array<std::array<float, 3>, 8> &v) {
    static constexpr float parts[6][9] = {{-1.9f, 12, 0, -2, -12, -2, 2, 0, 2}, {1.9f, 12, 0, -2, -12, -2, 2, 0, 2},
                                          {0, 12, 0, -4, 0, -2, 4, 12, 2},      {-5, 22, 0, -3, -10, -2, 1, 2, 2},
                                          {5, 22, 0, -1, -10, -2, 3, 2, 2},     {0, 24, 0, -4, 0, -4, 4, 8, 4}};
    const auto &a = parts[i];
    float cp = std::cos(p.pitch[i]), sp = std::sin(p.pitch[i]), cy = std::cos(p.yaw[i]), sy = std::sin(p.yaw[i]),
          cr = std::cos(p.roll[i]), sr = std::sin(p.roll[i]);
    float lean = crouch ? .45f * (1 - ka) : 0, drop = crouch ? 3 * (1 - ka) : 0, c = std::cos(yaw * deg),
          s = std::sin(yaw * deg);
    for (int n = 0; n < 8; ++n) {
        float x = (n & 1) ? a[6] + inflate : a[3] - inflate, y = (n & 2) ? a[7] + inflate : a[4] - inflate,
              z = (n & 4) ? a[8] + inflate : a[5] - inflate;
        float ay = y * cp + z * sp, az = z * cp - y * sp, bx = x * cy - az * sy, bz = x * sy + az * cy;
        float px = bx * cr - ay * sr + a[0], py = bx * sr + ay * cr + a[1], pz = bz + a[2];
        if (i >= 2) {
            float hy = py - 12, ny = hy * std::cos(lean) - pz * std::sin(lean);
            pz = hy * std::sin(lean) + pz * std::cos(lean);
            py = ny + 12;
        }
        py -= drop;
        v[n] = {(px * c - pz * s) * unit, py * unit, (px * s + pz * c) * unit};
    }
}
} // namespace vanta::modules::visualports::math
