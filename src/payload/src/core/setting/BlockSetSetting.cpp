#include "core/setting/BlockSetSetting.h"

#include <algorithm>

namespace dt {

BlockSetSetting::BlockSetSetting(Module* o, std::string n, std::vector<std::string> d)
    : Setting(o, std::move(n), SettingType::BlockSet), ids_(std::move(d)) {}

bool BlockSetSetting::contains(const std::string& id) const noexcept {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

void BlockSetSetting::toggle(const std::string& id) {
    auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) ids_.push_back(id);
    else                  ids_.erase(it);
}

bool BlockSetSetting::draw() { return false; } // ClickGui uses c_block_picker_setting

nlohmann::json BlockSetSetting::save() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : ids_) arr.push_back(s);
    return arr;
}

void BlockSetSetting::load(const nlohmann::json& j) {
    if (!j.is_array()) return;
    ids_.clear();
    for (const auto& e : j) {
        if (e.is_string()) ids_.push_back(e.get<std::string>());
    }
}

} // namespace dt
