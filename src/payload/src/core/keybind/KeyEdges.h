#pragma once
#include <array>
namespace dt {
// Tracks physical state even while input is captured, so closing a menu with a
// key held cannot manufacture a new press. A suppressed active hold releases once.
class KeyEdges {
    std::array<bool,256> physical_{};
    std::array<bool,256> active_{};
public:
    int update(unsigned vk,bool down,bool allowed) {
        if(vk==0 || vk>=physical_.size()) return 0;
        const bool pressed=down && !physical_[vk];
        physical_[vk]=down;
        if(active_[vk] && (!down || !allowed)) { active_[vk]=false; return -1; }
        if(allowed && pressed) { active_[vk]=true; return 1; }
        return 0;
    }
};
}
