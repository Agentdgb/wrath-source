#pragma once
#include <jni.h>
namespace vanta::modules::bloom {
struct AtmosphereSettings {
    bool enabled = false, sky = true, fog = true;
    int mode = 0, palette = 0;
    float stars = .65f, fogStart = 12, fogEnd = 128;
    bool rainbow = false, clouds = false, customTime = false;
    int top = 1, preset = 13, fogMode = 0;
    float strength = 1, cycle = 6, fogDensity = 0, worldTint = 0, time = 6000, presetDensity = .55f,
          presetStrength = .9f, detail = .45f;
};
extern AtmosphereSettings atmosphere;
void tickAtmosphere(float deltaMs);
const char *atmosphereStatus();
void stopAtmosphere();
void freeAtmosphere(JNIEnv *);
bool atmosphereSkyActive();
} // namespace vanta::modules::bloom
