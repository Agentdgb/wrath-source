#pragma once
#include <jni.h>

namespace vanta::modules::shieldbreaker {
struct Options {
    bool only_swords = true;
    bool switch_back = true;
    bool check_facing = true;
    bool strongest_axe = true;
    int return_delay_ms = 150;
};
void configure(Options options) noexcept;
bool has_pending_return() noexcept;
void pump_return(JNIEnv* env, jobject mc, float delta_ms);
void clear_pending_return(JNIEnv* env, bool restore);
void set_enabled(bool value) noexcept;
// Queries only; does not change inventory or attack state.
bool can_break(JNIEnv* env, jobject mc, jobject target);

// Canceled/failed attacks restore immediately. Successful attacks may queue
// their return so the selected axe remains visible for the configured interval.
class AttackSlot {
public:
    AttackSlot(JNIEnv* env, jobject mc);
    ~AttackSlot();
    void complete() noexcept { completed_ = true; }
    AttackSlot(const AttackSlot&) = delete;
    AttackSlot& operator=(const AttackSlot&) = delete;
private:
    JNIEnv* env_;
    jobject inventory_ = nullptr;
    jfieldID selected_ = nullptr;
    int original_ = -1;
    int axe_ = -1;
    Options options_;
    bool completed_ = false;
};
}
