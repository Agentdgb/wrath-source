#pragma once
#include <string_view>
namespace vanta::modules::triggerbot {
inline bool is_weapon_key(std::string_view key) noexcept {
    auto ends = [key](std::string_view suffix) {
        return key.size() >= suffix.size() && key.substr(key.size()-suffix.size()) == suffix;
    };
    return ends("_sword") || ends("_axe") || ends(".mace") || ends(".trident");
}
}
