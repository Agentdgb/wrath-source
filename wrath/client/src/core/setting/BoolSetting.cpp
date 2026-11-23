#include "core/setting/BoolSetting.h"

#include <imgui.h>

namespace vanta {

BoolSetting::BoolSetting(Module* o, std::string n, bool d)
    : Setting(o, std::move(n), SettingType::Bool), value_(d) {}

BoolSetting::BoolSetting(Module* o, std::string n, bool d, std::string description)
    : Setting(o, std::move(n), SettingType::Bool, std::move(description)), value_(d) {}

bool BoolSetting::draw() {
    return ImGui::Checkbox(name_.c_str(), &value_);
}

nlohmann::json BoolSetting::save() const {
    return value_;
}

void BoolSetting::load(const nlohmann::json& j) {
    if (j.is_boolean()) value_ = j.get<bool>();
}

} // namespace vanta
