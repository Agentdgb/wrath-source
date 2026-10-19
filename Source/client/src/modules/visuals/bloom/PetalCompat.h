#pragma once
#include "BloomCompat.h"
#include <functional>
namespace vanta::modules::bloom::petals {
struct SyncController {
    static inline int l = 1;
};
struct BatchArrayCache {
    static inline Array<float> am = Array<float>(0);
};
struct RenderPipelines {
    static constexpr int GUI_TEXTURED = 0;
};
struct GuiMatrix {
    std::array<float, 6> m{1, 0, 0, 1, 0, 0};
    std::vector<std::array<float, 6>> stack;
    void pushMatrix() { stack.push_back(m); }
    void popMatrix() {
        if (stack.empty())
            throw std::runtime_error("Petal matrix stack underflow");
        m = stack.back();
        stack.pop_back();
    }
    void translate(float x, float y) {
        m[4] += m[0] * x + m[2] * y;
        m[5] += m[1] * x + m[3] * y;
    }
    void rotate(float a) {
        float c = std::cos(a), s = std::sin(a);
        auto old = m;
        m[0] = old[0] * c + old[2] * s;
        m[1] = old[1] * c + old[3] * s;
        m[2] = -old[0] * s + old[2] * c;
        m[3] = -old[1] * s + old[3] * c;
    }
    void scale(float x, float y) {
        m[0] *= x;
        m[1] *= x;
        m[2] *= y;
        m[3] *= y;
    }
    std::array<float, 2> point(float x, float y) const {
        return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
    }
};
using Matrix3x2fStack = GuiMatrix *;
struct GuiCanvas {
    GuiMatrix matrices;
    std::function<void(const std::array<std::array<float, 2>, 4> &, float, float, float, float, std::uint32_t)> emit;
    Matrix3x2fStack getMatrices() { return &matrices; }
    void drawTexture(int, int, float x, float y, float u, float v, int w, int h, int sourceW, int sourceH, int atlasW,
                     int atlasH, int color) {
        std::array<std::array<float, 2>, 4> p = {matrices.point(x, y), matrices.point(x + w, y),
                                                 matrices.point(x + w, y + h), matrices.point(x, y + h)};
        if (emit)
            emit(p, u / atlasW, v / atlasH, (u + sourceW) / atlasW, (v + sourceH) / atlasH,
                 static_cast<std::uint32_t>(color));
    }
};
using DrawContext = GuiCanvas *;
template <class F> struct ScopeExit {
    F fn;
    ~ScopeExit() { fn(); }
};
template <class F> ScopeExit<F> finally(F fn) { return {fn}; }
void b();
void c(DrawContext, int, int, float, int, int, std::int64_t);
bool d();
void reset();
} // namespace vanta::modules::bloom::petals
