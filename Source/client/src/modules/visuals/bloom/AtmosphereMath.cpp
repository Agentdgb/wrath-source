#include "AtmosphereMath.h"
namespace vanta::modules::bloom {
namespace {
std::array<float, 3> rgb(std::uint32_t c) {
    return {float(c >> 16 & 255) / 255, float(c >> 8 & 255) / 255, float(c & 255) / 255};
}
std::uint32_t blendColor(std::uint32_t old, std::array<float, 3> target, float alpha) {
    auto original = rgb(old);
    std::uint32_t result = old & 0xFF000000;
    for (int k = 0; k < 3; ++k)
        result |=
            std::uint32_t(std::clamp(std::lround((original[k] + (target[k] - original[k]) * alpha) * 255), 0L, 255L))
            << (16 - 8 * k);
    return result;
}
float brightness(std::uint32_t color) {
    auto v = rgb(color);
    return std::max({v[0], v[1], v[2]});
}
std::array<float, 3> scale(std::array<float, 3> c, float factor) {
    for (auto &x : c)
        x *= factor;
    return c;
}
float skyAngle(float ticks) {
    double x = std::fmod(double(ticks) - 6000 + 24000, 24000) / 24000;
    auto curve = [](double t, double p, double q) {
        double u = 1 - t;
        return 3 * u * u * t * p + 3 * u * t * t * q + t * t * t;
    };
    double a = 0, b = 1;
    for (int k = 0; k < 28; ++k) {
        double m = (a + b) * .5;
        if (curve(m, .362, .638) < x)
            a = m;
        else
            b = m;
    }
    return float(curve((a + b) * .5, .241, .759) * 360);
}
} // namespace
void AtmosphereModel::update(const AtmosphereSettings &s, float dt, double clock, std::array<float, 3> theme) {
    dt = std::clamp(dt, 0.f, .1f);
    float smooth = 1 - std::exp(-6 * dt);
    float wanted = s.enabled && s.sky ? s.strength : 0;
    float oldFade = fade;
    fade += (wanted - fade) * smooth;
    if (std::abs(wanted - fade) < .001f)
        fade = wanted;
    static constexpr std::uint32_t palette[] = {10120959, 3832831, 3596512, 4644954, 16730955, 16751162, 16740039};
    auto baseTarget = s.palette == 7 ? theme : rgb(palette[std::clamp(s.palette, 0, 6)]);
    auto topTarget = baseTarget;
    float phase = float(std::fmod(clock, std::max(.1f, s.cycle)) / std::max(.1f, s.cycle));
    if (s.rainbow)
        baseTarget = rgb(MathHelper::hsvToRgb(phase, .65f, 1));
    if (s.top == 1)
        topTarget = s.rainbow ? rgb(MathHelper::hsvToRgb(phase + .18f, .65f, 1)) : baseTarget;
    else if (s.top == 2)
        topTarget = rgb(461338);
    else if (s.top >= 3 && s.top <= 9)
        topTarget = rgb(palette[s.top - 3]);
    else if (s.top == 10)
        topTarget = theme;
    if (!initialized || oldFade <= .002f) {
        base = baseTarget;
        top = topTarget;
        density = s.fogDensity;
        tint = s.worldTint;
    } else {
        for (int k = 0; k < 3; ++k) {
            base[k] += (baseTarget[k] - base[k]) * smooth;
            top[k] += (topTarget[k] - top[k]) * smooth;
        }
        density += (s.fogDensity - density) * smooth;
        tint += (s.worldTint - tint) * smooth;
    }
    auto &v = weather;
    sky::State target;
    sky::profile(target.cs, std::clamp(s.preset, 0, 12), s.presetDensity);
    if (!initialized) {
        for (int k = 0; k < 31; ++k)
            v.cs[k] = target.cs[k];
        for (int k = 0; k < 13; ++k)
            v.pw[k] = s.preset == k ? 1.f : 0.f;
        v.da = s.presetStrength;
        v.di = s.detail;
        initialized = true;
    }
    float active = s.enabled && s.sky && s.preset < 13 && overworld ? 1.f : 0.f;
    v.cv += (active - v.cv) * (1 - std::exp(-dt * 1.7f));
    if (v.cv < .0001f && active == 0)
        v.cv = 0;
    v.ep += (active - v.ep) * (1 - std::exp(-dt * 1.7f));
    v.da += (s.presetStrength - v.da) * (1 - std::exp(-dt * 3));
    v.di = s.detail;
    v.dd += dt;
    for (int k = 0; k < 13; ++k)
        v.pw[k] += ((s.preset == k ? 1.f : 0.f) - v.pw[k]) * (1 - std::exp(-dt * 1.3f));
    if (s.preset < 13)
        for (int k = 0; k < 31; ++k) {
            float difference = target.cs[k] - v.cs[k];
            if (k == 19 || k == 20)
                difference = MathHelper::wrapDegrees(difference);
            v.cs[k] += difference * (1 - std::exp(-dt * 1.4f));
        }
    v.dj = 0;
    for (int k : {6, 8, 9}) {
        sky::profile(target.cs, k, s.presetDensity);
        v.dj += target.cs[21] * v.pw[k];
    }
    timeFade += ((s.enabled && s.customTime && overworld ? 1.f : 0.f) - timeFade) * (1 - std::exp(-dt * 3));
    if (timeFade < .0001f && !s.customTime)
        timeFade = 0;
}
std::uint32_t AtmosphereModel::color(const AtmosphereSettings &s, SkyAttribute key, std::uint32_t original) const {
    auto value = original;
    auto &v = weather;
    auto &w = v.cs;
    float amount = std::clamp(v.cv * v.da * 1.45f, 0.f, 1.f);
    if (amount > .002f && overworld) {
        float minimum = w[22], day = std::max(brightness(original), minimum);
        float flicker = .5f + .5f * std::sin(v.dd * .6);
        std::array<float, 3> target{};
        bool replace = true;
        if (key == SkyAttribute::Sky)
            target = scale({w[0], w[1], w[2]}, (1 - w[26] * (1 - flicker)) * std::max(v.eu, minimum));
        else if (key == SkyAttribute::Fog) {
            float flicker2 = .5f + .5f * float(.6 * std::sin(v.dd * .37) + .3 * std::sin(v.dd * 1.13 + 1.7) +
                                               .1 * std::sin(v.dd * 2.9 + .4));
            target = scale({w[3], w[4], w[5]}, (1 - .5f * w[26] * (1 - flicker) + w[28] * flicker2 * v.db) * day);
        } else if (key == SkyAttribute::Cloud)
            target = scale({w[10], w[11], w[12]}, day);
        else if (key == SkyAttribute::LightColor) {
            target = {w[13], w[14], w[15]};
            amount *= .85f;
        } else if (key == SkyAttribute::Sunrise)
            target = {w[6], w[7], w[8]};
        else
            replace = false;
        if (replace) {
            value = blendColor(value, target, amount);
            if (key == SkyAttribute::Cloud) {
                auto a = std::uint32_t(
                    std::clamp(std::lround(float(original >> 24) * (1 - amount * (1 - w[23]))), 0L, 255L));
                value = value & 0xFFFFFF | a << 24;
            }
            if (key == SkyAttribute::Sunrise) {
                float pulse = 1 - w[27] * (.5f - .5f * float(std::sin(v.dd * 1.7) * std::sin(v.dd * .73)));
                float alpha = w[9] * pulse * std::max(v.eu, minimum);
                auto a = std::uint32_t(std::clamp(
                    std::lround((original >> 24) + (alpha * 255 - float(original >> 24)) * amount), 0L, 255L));
                value = value & 0xFFFFFF | a << 24;
            }
        }
    }
    if (fade <= .002f)
        return value;
    std::uint32_t user = value;
    if (key == SkyAttribute::Sky && s.top != 0) {
        auto target = top;
        if (s.mode == 0) {
            auto c = rgb(original);
            for (int k = 0; k < 3; ++k)
                target[k] *= c[k];
        }
        user = blendColor(original, target, fade);
    } else if (key == SkyAttribute::Cloud && s.clouds)
        user = blendColor(original, scale(base, brightness(original)), .8f * fade);
    else
        return value;
    if (amount > .002f)
        return blendColor(user, rgb(value), amount);
    return user;
}
float AtmosphereModel::number(const AtmosphereSettings &s, SkyAttribute key, float original) const {
    auto &v = weather;
    auto &w = v.cs;
    float weight = overworld ? v.cv * v.da : 0, value = original;
    if (weight > .002f) {
        if (key == SkyAttribute::FogStart)
            value = original + (w[16] - original) * weight;
        else if (key == SkyAttribute::FogEnd || key == SkyAttribute::SkyFogEnd || key == SkyAttribute::CloudFogEnd) {
            float target = key == SkyAttribute::SkyFogEnd     ? w[18]
                           : key == SkyAttribute::CloudFogEnd ? w[17] * 2
                                                              : w[17];
            value = 1 / ((1 - weight) / std::max(original, 1.f) + weight / std::max(target, 1.f));
        } else if (key == SkyAttribute::Stars) {
            float coverage = v.pw[3] + v.pw[4] + v.pw[10] + v.pw[11] + v.pw[6] + v.pw[8] + v.pw[9];
            float target = w[21] * v.dk * (1 - std::min(1.f, coverage));
            value = original + (target - original) * weight;
        } else if (key == SkyAttribute::LightFactor) {
            float target = w[24] * (original + (1 - original) * w[22] * w[30]);
            value = original + (target - original) * std::min(1.f, weight * w[25]);
        }
    }
    if (key == SkyAttribute::SunAngle || key == SkyAttribute::MoonAngle || key == SkyAttribute::StarAngle) {
        float target = key == SkyAttribute::MoonAngle ? w[20] : w[19];
        float blend = v.cv * (1 - timeFade) * w[30];
        value = original + MathHelper::wrapDegrees(target - original) * blend;
        if (timeFade > .0001f) {
            target = skyAngle(s.time) + (key == SkyAttribute::MoonAngle ? 180 : 0);
            value += MathHelper::wrapDegrees(target - value) * timeFade;
        }
        return value;
    }
    if (key == SkyAttribute::Stars) {
        float user = std::max(original, s.stars * fade);
        return user + (value - user) * std::min(1.f, weight);
    }
    if (s.fog && !submerged && (key == SkyAttribute::FogStart || key == SkyAttribute::FogEnd)) {
        float user = original;
        if (s.fogMode == 1) {
            float target = key == SkyAttribute::FogStart ? std::min(s.fogStart, s.fogEnd - 1) : std::max(1.f, s.fogEnd);
            user = original + (target - original) * fade;
        } else {
            float amount = std::clamp(fade * density, 0.f, 1.f);
            if (key == SkyAttribute::FogStart)
                user = original * (1 - amount);
            else if (original > 32)
                user = original * std::pow(32 / original, amount);
        }
        return user + (value - user) * std::min(1.f, weight);
    }
    return value;
}
} // namespace vanta::modules::bloom
