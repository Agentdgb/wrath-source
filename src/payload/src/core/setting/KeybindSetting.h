#pragma once
#include "core/setting/Setting.h"

namespace dt {

enum class BindMode : std::uint8_t { Toggle, Hold, Once, Disabled, Always };

class KeybindSetting : public Setting {
public:
    KeybindSetting(Module* owner, std::string name, int defaultVK = 0, BindMode mode = BindMode::Toggle);

    int      vk()    const noexcept { return vk_; }
    BindMode mode()  const noexcept { return mode_; }
    void     setVK(int vk) noexcept { vk_ = vk > 0 && vk < 256 ? vk : 0; }
    void     setMode(BindMode m) noexcept { mode_ = m == BindMode::Always ? BindMode::Toggle : m; }

    // ClickGui uses these to display and enter capture mode.
    bool          capturing() const noexcept { return capturing_; }
    void          beginCapture() noexcept    { capturing_ = true; }
    void          endCapture()   noexcept    { capturing_ = false; }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;
private:
    int      vk_;
    BindMode mode_;
    bool     capturing_ = false;
};

} // namespace dt
