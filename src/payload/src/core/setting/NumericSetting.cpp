#include "core/setting/NumericSetting.h"

#include <imgui.h>
#include "core/setting/NumericPrecision.h"
#include <cstdio>

namespace vanta {

IntSetting::IntSetting(Module* o, std::string n, int d, int mn, int mx)
    : Setting(o, std::move(n), SettingType::Int), value_(d), min_(mn), max_(mx) {}

bool IntSetting::draw() {
    return ImGui::SliderInt(name_.c_str(), &value_, min_, max_);
}
nlohmann::json IntSetting::save() const { return value_; }
void IntSetting::load(const nlohmann::json& j) {
    if (j.is_number_integer()) set(j.get<int>());
}

FloatSetting::FloatSetting(Module* o, std::string n, float d, float mn, float mx, float step)
    : Setting(o, std::move(n), SettingType::Float), value_(d), min_(mn), max_(mx), step_(step) {}

bool FloatSetting::draw() {
    char format[16];
    std::snprintf(format, sizeof(format), "%%.%df", numericDecimalPlaces(step_));
    return ImGui::SliderFloat(name_.c_str(), &value_, min_, max_, format);
}
nlohmann::json FloatSetting::save() const { return value_; }
void FloatSetting::load(const nlohmann::json& j) {
    if (j.is_number()) set(j.get<float>());
}

} // namespace vanta
