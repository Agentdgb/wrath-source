#pragma once
#include "core/setting/Setting.h"

namespace vanta {

struct RGBA { float r, g, b, a; };

class ColorSetting : public Setting {
public:
    ColorSetting(Module* owner, std::string name, RGBA defaultValue);

    RGBA value() const noexcept { return v_; }
    void set(RGBA c) noexcept   { v_ = c; }

    // Packed 0xAARRGGBB helper for HUD draws that want an ImU32.
    std::uint32_t packed() const noexcept;

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    RGBA v_;
};

} // namespace vanta
