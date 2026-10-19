#include "modules/visuals/bloom/AtmosphereMath.h"
#include <cstdlib>
#include <iostream>
using namespace vanta::modules::bloom;
void check(bool okay, const char *message) {
    if (!okay) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void settle(AtmosphereModel &m, const AtmosphereSettings &s) {
    for (int i = 0; i < 300; ++i)
        m.update(s, 1.f / 60, 12, {.3f, .6f, .9f});
}
bool close(float a, float b) { return std::abs(a - b) < .01f; }
int main() {
    AtmosphereSettings s;
    AtmosphereModel m;
    settle(m, s);
    check(m.color(s, SkyAttribute::Sky, 0xFF789ABC) == 0xFF789ABC, "Disabled sky changed");
    check(close(m.number(s, SkyAttribute::FogEnd, 192), 192), "Disabled fog changed");
    s.enabled = true;
    s.mode = 1;
    s.palette = 0;
    s.top = 1;
    s.fogDensity = 1;
    settle(m, s);
    check(m.color(s, SkyAttribute::Sky, 0xFF000000) == 0xFF9A6EFF, "Original palette/solid sky mismatch");
    check(close(m.number(s, SkyAttribute::FogStart, 32), 0), "Density start mismatch");
    check(close(m.number(s, SkyAttribute::FogEnd, 192), 32), "Density end mismatch");
    m.submerged = true;
    check(close(m.number(s, SkyAttribute::FogEnd, 192), 192), "Fluid fog changed");
    m.submerged = false;
    s.fogMode = 1;
    s.fogStart = 15;
    s.fogEnd = 96;
    settle(m, s);
    check(close(m.number(s, SkyAttribute::FogStart, 32), 15) && close(m.number(s, SkyAttribute::FogEnd, 192), 96),
          "Explicit distances mismatch");
    s.top = 0;
    settle(m, s);
    check(m.color(s, SkyAttribute::Sky, 0xFF123456) == 0xFF123456, "Vanilla top changed");
    s.customTime = true;
    s.time = 6000;
    settle(m, s);
    check(std::abs(MathHelper::wrapDegrees(m.number(s, SkyAttribute::SunAngle, 35))) < .01f, "Noon sun mismatch");
    check(std::abs(MathHelper::wrapDegrees(m.number(s, SkyAttribute::MoonAngle, 35) - 180)) < .01f,
          "Noon moon mismatch");
    s.time = 18000;
    settle(m, s);
    check(std::abs(MathHelper::wrapDegrees(m.number(s, SkyAttribute::SunAngle, 35) - 180)) < .01f,
          "Midnight sun mismatch");
    s.customTime = false;
    for (int profile = 0; profile < 13; ++profile) {
        s.preset = profile;
        settle(m, s);
        sky::State expected;
        sky::profile(expected.cs, profile, s.presetDensity);
        check(std::abs(m.weather.cs[0] - expected.cs[0]) < .02f, "Preset array alias/interpolation mismatch");
        for (int i = 0; i < 14; ++i) {
            auto key = static_cast<SkyAttribute>(i);
            check(std::isfinite(m.number(s, key, 100)), "Nonfinite environment attribute");
        }
    }
    s.enabled = false;
    settle(m, s);
    settle(m, s);
    check(m.fade == 0 && m.weather.cv == 0 && m.timeFade < .0001f, "Disable fade did not finish");
    check(m.color(s, SkyAttribute::Sky, 0xFF789ABC) == 0xFF789ABC, "Sky did not return to vanilla");
    check(close(m.number(s, SkyAttribute::SunAngle, 35), 35), "Time did not return to vanilla");
    std::cout << "Atmosphere passed: colors, fog, submersion, visual time, 13 preset transitions and disable.\n";
}
