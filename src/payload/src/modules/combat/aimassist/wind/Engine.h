#pragma once
#include "aimassist.h"
namespace dt::modules::aimassist::wind {
class Engine {
    Runtime legacy;
    const char *state_ = "Paused";

  public:
    ~Engine();
    void step(JNIEnv *env, const SettingsView &settings);
    void pause();
    const char *status() const noexcept { return state_; }
};
} // namespace dt::modules::aimassist::wind
