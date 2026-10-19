#pragma once
#include "Atmosphere.h"
#include "native/SkyModel.h"
namespace vanta::modules::bloom {
enum class SkyAttribute {
    Sky,
    Fog,
    Cloud,
    Sunrise,
    LightColor,
    FogStart,
    FogEnd,
    SkyFogEnd,
    CloudFogEnd,
    Stars,
    SunAngle,
    MoonAngle,
    StarAngle,
    LightFactor
};
struct AtmosphereModel {
    float fade = 0, density = 0, tint = 0, timeFade = 0;
    bool initialized = false, overworld = true, submerged = false;
    std::array<float, 3> base{1, 1, 1}, top{1, 1, 1};
    sky::State weather;
    void update(const AtmosphereSettings &, float seconds, double clock, std::array<float, 3> theme);
    std::uint32_t color(const AtmosphereSettings &, SkyAttribute, std::uint32_t original) const;
    float number(const AtmosphereSettings &, SkyAttribute, float original) const;
};
AtmosphereModel &atmosphereModel();
float atmosphereFrameDelta();
float atmosphereNaturalLight();
} // namespace vanta::modules::bloom
