#include "core/setting/Setting.h"

namespace dt {

Setting::Setting(Module* owner, std::string name, SettingType type)
    : owner_(owner), name_(std::move(name)), type_(type) {}

Setting::Setting(Module* owner, std::string name, SettingType type, std::string description)
    : owner_(owner), name_(std::move(name)), description_(std::move(description)), type_(type) {}

} // namespace dt
