#include "core/setting/RangeSetting.h"

#include <imgui.h>

#include <algorithm>

namespace vanta {

RangeSetting::RangeSetting(Module* o, std::string n,
                           int defMin, int defMax,
                           int absMin, int absMax)
    : Setting(o, std::move(n), SettingType::Range),
      min_(defMin), max_(defMax),
      absoluteMin_(absMin), absoluteMax_(absMax)
{
    set(defMin, defMax);   // normalises the invariant on construction too
}

void RangeSetting::set(int newMin, int newMax) noexcept {
    if (newMin > newMax) std::swap(newMin, newMax);
    min_ = std::clamp(newMin, absoluteMin_, absoluteMax_);
    max_ = std::clamp(newMax, absoluteMin_, absoluteMax_);
}

bool RangeSetting::draw() {
    // ImGui fallback — the real UI lives in the vgui c_range_slider_setting.
    // This is only exercised by any leftover ImGui-driven flow.
    int a = min_, b = max_;
    if (ImGui::DragIntRange2(name_.c_str(), &a, &b, 1.0f, absoluteMin_, absoluteMax_)) {
        set(a, b);
        return true;
    }
    return false;
}

nlohmann::json RangeSetting::save() const {
    return { { "min", min_ }, { "max", max_ } };
}

void RangeSetting::load(const nlohmann::json& j) {
    if (!j.is_object()) return;
    const auto a = j.value("min", min_);
    const auto b = j.value("max", max_);
    set(a, b);
}

} // namespace vanta
