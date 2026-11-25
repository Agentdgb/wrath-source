#pragma once
#include "core/setting/ColorSetting.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include <array>
#include <functional>
#include <jni.h>
namespace dt::modules::bloom {
struct WorldSettings {
    bool enabled = false, stars = true, spark = false, flame = false, runes = false, snow = false, theme = true;
    float density = .35f;
    RGBA color{.8f, .5f, 1, 1};
};
extern WorldSettings worldSettings;
struct TargetSettings {
    bool enabled = false, glow = true;
    int mode = 4, on = 0;
    float scale = 1, opacity = .9f, speed = 1.5f;
    RGBA color{.78f, .49f, 1, .9f};
};
extern TargetSettings targetSettings;
using Point = std::array<double, 3>;
struct Canvas {
    std::function<void(Point, Point, Point, Point, RGBA)> quad;
    std::function<void(Point, Point, float, float, RGBA, RGBA, int)> line;
    std::function<void(Point, float, float, float, int, RGBA)> sprite;
    std::function<void(const std::array<Point, 4> &, const std::array<RGBA, 4> &,
                       const std::array<std::array<float, 2>, 4> &, float)>
        mesh;
    std::function<void(bool)> layer;
};
bool needsWorld();
const char *runtimeStatus();
void worldFrame(JNIEnv *, jobject world, jobject self, const hitfx::render_frame &, const Canvas &);
void resetWorld();
void shutdown();
} // namespace dt::modules::bloom
