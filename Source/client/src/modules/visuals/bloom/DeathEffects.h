#pragma once
#include <jni.h>
namespace vanta::modules::bloom {
struct DeathSettings {
    bool enabled = false, theme = true, hide = true, sounds = true, flash = false, own = false;
    int mode = 1, max = 4, on = 0, color = 0xC080FF;
    float scale = 1, amount = 1;
};
extern DeathSettings deathSettings;
void deathPacket(JNIEnv *, jobject, jobject, jobject);
bool deathInterested();
void setDeathInterest(bool);
void installDeathHide(JNIEnv *);
const char *deathHideError();
void freeDeathHide(JNIEnv *);
struct EntityView;
namespace death {
void a();
void b();
void h();
void g(EntityView *, bool);
bool i(int);
bool visible();
int active();
void configure(const DeathSettings &);
} // namespace death
} // namespace vanta::modules::bloom
