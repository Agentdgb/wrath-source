#pragma once
#include "util/StringScrub.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dt {

class Module;

enum class SettingType : std::uint8_t { Bool, Int, Float, Enum, Keybind, Color, String, Range, SlotMap, BlockSet };

class Setting {
public:
    Setting(Module* owner, std::string name, SettingType type);
    Setting(Module* owner, std::string name, SettingType type, std::string description);
    virtual ~Setting() {
        dt::util::scrubString(name_);
        dt::util::scrubString(label_);
        dt::util::scrubString(description_);
    }

    std::string_view name()  const noexcept { return name_; }

    // Presentation label is independent of the profile key returned by name().
    std::string_view label() const noexcept { return label_.empty() ? std::string_view(name_) : std::string_view(label_); }
    void setLabel(std::string text) { label_ = std::move(text); }

    // Optional one-line explanation of what the setting does. The ClickGui
    // shows it as a tooltip when the row is hovered; empty means no tooltip.
    std::string_view description() const noexcept { return description_; }
    void setDescription(std::string d) { description_ = std::move(d); }
    SettingType      type()  const noexcept { return type_; }
    Module*          owner() const noexcept { return owner_; }

    // ImGui widget. Returns true when the user changed the value in this frame
    // (triggers config auto-save).
    virtual bool draw() = 0;
    virtual nlohmann::json save() const = 0;
    virtual void           load(const nlohmann::json& j) = 0;

    // Optional visibility predicate — settings can hide themselves when
    // another setting's value makes them irrelevant.
    void setVisibleIf(std::function<bool()> pred) { visible_ = std::move(pred); }
    bool visible() const { return !visible_ || visible_(); }

protected:
    Module*                       owner_;
    std::string                   name_;
    std::string                   label_;
    std::string                   description_;
    SettingType                   type_;
    std::function<bool()>         visible_;
};

} // namespace dt
