#pragma once
#include "core/setting/ColorSetting.h"
#include <array>
#include <jni.h>
#include <vector>
namespace vanta::modules::visualports {
struct SwingConfig {
    bool enabled = false, offhand = true, combatOnly = false, slow = false;
    int style = 0;
    float tempo = 1, intensity = 1, speed = .25f;
};
struct SpiritConfig {
    bool enabled = false, pose = true, crest = true, seal = true, own = false, walls = false, manual = false;
    int look = 0;
    float life = 2.4f, ascent = 1.6f, opacity = .65f;
    RGBA body{}, aura{};
};
struct CharacterConfig {
    bool enabled = false, friends = false, held = false;
    int model = 0, detail = 1;
};
extern SwingConfig swing;
extern SpiritConfig spirit;
extern CharacterConfig character;
extern bool blurEnabled;
extern float blurStrength;
void tick();
void receive(JNIEnv *, jobject);
void afterWorld(JNIEnv *);
void shutdownGl();
void shutdownCache(); // after JNIHook_Shutdown and callback drain
bool prepareShutdown();
bool isStopping();
bool prepareCharacterShutdown(JNIEnv *);
const char *swingStatus();
const char *worldStatus();
void installCharacter(JNIEnv *);
void shutdownCharacter(JNIEnv *);
const char *characterStatus();
void beforeHand(JNIEnv *, jobject hand);
void afterHand();
namespace math {
constexpr float pi = 3.14159265358979323846f, deg = pi / 180;
float crest(float t, float peak);
float retention(float strength, double seconds);
struct Op {
    int axis;
    float x, y, z;
}; // -1 = translate, 0/1/2 = rotate X/Y/Z in degrees
std::vector<Op> swingPose(int style, float progress, int side, float intensity);
struct SpiritPose {
    float pitch[6]{}, yaw[6]{}, roll[6]{};
};
float ascend(float t);
float dissolve(float t, float tier);
SpiritPose spiritPose(float walk, float stride, float headPitch, float headYaw, bool crouch, float ka);
void spiritCorners(int part, float inflate, const SpiritPose &, bool crouch, float ka, float unit, float bodyYaw,
                   std::array<std::array<float, 3>, 8> &);
} // namespace math
} // namespace vanta::modules::visualports
