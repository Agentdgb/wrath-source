#include "core/setting/ColorSetting.h"

#include <imgui.h>
#include <algorithm>

namespace dt {

ColorSetting::ColorSetting(Module* o, std::string n, RGBA d)
    : Setting(o, std::move(n), SettingType::Color), v_(d) {}

std::uint32_t ColorSetting::packed() const noexcept {
    auto clamp01 = [](float x){ return std::clamp(x, 0.f, 1.f); };
    auto r = static_cast<std::uint32_t>(clamp01(v_.r) * 255.f);
    auto g = static_cast<std::uint32_t>(clamp01(v_.g) * 255.f);
    auto b = static_cast<std::uint32_t>(clamp01(v_.b) * 255.f);
    auto a = static_cast<std::uint32_t>(clamp01(v_.a) * 255.f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

bool ColorSetting::draw() {
    float col[4] = {v_.r, v_.g, v_.b, v_.a};
    bool changed = ImGui::ColorEdit4(name_.c_str(), col,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    if (changed) v_ = {col[0], col[1], col[2], col[3]};
    return changed;
}

nlohmann::json ColorSetting::save() const {
    return {{"r", v_.r}, {"g", v_.g}, {"b", v_.b}, {"a", v_.a}};
}
void ColorSetting::load(const nlohmann::json& j) {
    if (!j.is_object()) return;
    v_ = {j.value("r", v_.r), j.value("g", v_.g), j.value("b", v_.b), j.value("a", v_.a)};
}

} // namespace dt
