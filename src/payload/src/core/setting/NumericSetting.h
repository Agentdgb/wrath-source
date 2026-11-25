#pragma once
#include "core/setting/Setting.h"

namespace dt {

class IntSetting : public Setting {
public:
    IntSetting(Module* owner, std::string name, int defaultValue, int min, int max);

    int  value() const noexcept { return value_; }
    void set(int v) noexcept    { value_ = v < min_ ? min_ : (v > max_ ? max_ : v); }
    int  min()   const noexcept { return min_; }
    int  max()   const noexcept { return max_; }
    operator int() const noexcept { return value_; }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    int value_, min_, max_;
};

class FloatSetting : public Setting {
public:
    FloatSetting(Module* owner, std::string name, float defaultValue, float min, float max, float step = 0.1f);

    float value() const noexcept { return value_; }
    void  set(float v) noexcept  { value_ = v < min_ ? min_ : (v > max_ ? max_ : v); }
    float min()   const noexcept { return min_; }
    float max()   const noexcept { return max_; }
    float step()  const noexcept { return step_; }
    operator float() const noexcept { return value_; }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    float value_, min_, max_, step_;
};

} // namespace dt
