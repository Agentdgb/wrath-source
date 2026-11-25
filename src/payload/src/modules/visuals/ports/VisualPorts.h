#pragma once
#include "core/setting/ColorSetting.h"
#include <array>
#include <jni.h>
#include <vector>
namespace dt::modules::visualports {
struct SwingConfig {
    bool enabled = false, offhand = true, combatOnly = false;
    int style = 0;
    float tempo = 1, intensity = 1;
};
extern SwingConfig swing;
void tick();
void afterWorld(JNIEnv *);
void shutdownGl();
void shutdownCache(); // after JNIHook_Shutdown and callback drain
bool prepareShutdown();
bool isStopping();
const char *swingStatus();
const char *worldStatus();
bool beforeHand(JNIEnv *, jobject player, jobject hand, float progress, jobject matrices);
void afterHand(JNIEnv *, jobject matrices, bool transformed);
namespace math {
constexpr float pi = 3.14159265358979323846f, deg = pi / 180;
float crest(float t, float peak);
struct Op {
    int axis;
    float x, y, z;
}; // -1 = translate, 0/1/2 = rotate X/Y/Z in degrees
std::vector<Op> swingPose(int style, float progress, int side, float intensity);
} // namespace math
} // namespace dt::modules::visualports
