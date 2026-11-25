#include "core/setting/EnumSetting.h"

#include <imgui.h>

#include "util/Log.h"

namespace dt {

EnumSetting::EnumSetting(Module* o, std::string n, std::initializer_list<const char*> opts, int def)
    : Setting(o, std::move(n), SettingType::Enum), index_(def) {
    options_.reserve(opts.size());
    for (auto* s : opts) if (s) options_.emplace_back(s);
    if (options_.empty()) options_.emplace_back("Default");
    if (index_ < 0 || index_ >= static_cast<int>(options_.size())) index_ = 0;
}

EnumSetting::EnumSetting(Module* o, std::string n, const char* const* opts, std::size_t count, int def)
    : Setting(o, std::move(n), SettingType::Enum), index_(def) {
    options_.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        if (opts && opts[i]) options_.emplace_back(opts[i]);
    if (options_.empty()) options_.emplace_back("Default");
    if (index_ < 0 || index_ >= static_cast<int>(options_.size())) index_ = 0;
}

bool EnumSetting::draw() {
    bool changed = false;
    if (ImGui::BeginCombo(name_.c_str(), options_[index_].c_str())) {
        for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
            const bool sel = (i == index_);
            if (ImGui::Selectable(options_[i].c_str(), sel)) { index_ = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

nlohmann::json EnumSetting::save() const { return options_[index_]; }

void EnumSetting::load(const nlohmann::json& j) {
    if (!j.is_string()) return;
    auto s = j.get<std::string>();
    for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
        if (options_[i] == s) { index_ = i; return; }
    }

    /* @an option that no longer exists is worth a line.

       The value is stored by NAME, so dropping or renaming an option leaves the
       old profile pointing at nothing -- and the silent outcome is that the
       setting quietly reverts to its default and the user is left wondering why
       their choice changed. Nothing is broken, but nothing said so either. */
    VLOG_WARN("cfg", "setting '{}': saved value '{}' is no longer an option, keeping '{}'",
              name_, s, options_.empty() ? std::string{} : options_[index_]);
}

} // namespace dt
