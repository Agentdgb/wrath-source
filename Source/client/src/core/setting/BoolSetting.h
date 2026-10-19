#pragma once
#include "core/setting/Setting.h"

namespace vanta {

class BoolSetting : public Setting {
public:
    BoolSetting(Module* owner, std::string name, bool defaultValue);
    // Same, carrying a description the ClickGui shows as a hover tooltip.
    BoolSetting(Module* owner, std::string name, bool defaultValue, std::string description);
    bool  value() const noexcept { return value_; }
    void  set(bool v) noexcept   { value_ = v; }
    explicit operator bool() const noexcept { return value_; }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    bool value_;
};

} // namespace vanta
