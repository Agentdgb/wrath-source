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
} // namespace vanta::modules::visualports::math
