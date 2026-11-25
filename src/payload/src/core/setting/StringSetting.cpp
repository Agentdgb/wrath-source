#include "core/setting/StringSetting.h"

#include <imgui.h>

namespace dt {

StringSetting::StringSetting(Module* o, std::string n, std::string d, std::size_t maxLen)
    : Setting(o, std::move(n), SettingType::String), v_(std::move(d)), maxLen_(maxLen) {
    if (v_.size() > maxLen_) v_.resize(maxLen_);
}

bool StringSetting::draw() {
    v_.resize(maxLen_, '\0');
    bool changed = ImGui::InputText(name_.c_str(), v_.data(), maxLen_);
    if (changed) v_.resize(std::strlen(v_.c_str()));
    return changed;
}

nlohmann::json StringSetting::save() const { return v_; }
void StringSetting::load(const nlohmann::json& j) {
    if (j.is_string()) set(j.get<std::string>());
}

} // namespace dt
