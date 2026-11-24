#pragma once
#include "core/setting/Setting.h"

namespace vanta {

class StringSetting : public Setting {
public:
    StringSetting(Module* owner, std::string name, std::string defaultValue, std::size_t maxLen = 256);

    const std::string& value() const noexcept { return v_; }
    void set(std::string s) noexcept          { v_ = std::move(s); if (v_.size() > maxLen_) v_.resize(maxLen_); }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    std::string v_;
    std::size_t maxLen_;
};

} // namespace vanta
