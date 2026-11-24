#pragma once
#include <cstdint>
#include <string_view>

namespace vanta {

// Order here IS the ClickGui column order — do not sort alphabetically.
enum class Category : std::uint8_t {
    Combat,
    Visuals,
    Movement,
    Utilities,
    Misc,
    _Count,
};

constexpr std::string_view toString(Category c) noexcept {
    switch (c) {
        case Category::Combat:    return "Combat";
        case Category::Visuals:   return "Visuals";
        case Category::Movement:  return "Movement";
        case Category::Utilities: return "Utilities";
        case Category::Misc:      return "Misc";
        default:                  return "?";
    }
}

} // namespace vanta
