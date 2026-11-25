#pragma once
#include <cstdint>
#include <string_view>

namespace dt::hook {

// Every hook site's canonical identity. When adding a new site, add an entry
// here AND a matching sites/*.cpp registering it with HookRegistry.
enum class HookId : std::uint8_t {
    ClientTick,
    GameLoop,
    KeyPress,
    MouseButton,
    PacketSend,
    PacketReceive,
    RenderWorldPre,
    RenderWorldPost,
    RenderHud,
    EntityRender,
    PlayerMove,
    Jump,
    Attack,
    UseItem,
    Damage,
    Chat,
    ScreenOpen,
    ClickBlock,
    GetFov,
    EyeHeight,
    BlockOutline,
    _Count,
};

constexpr std::string_view toString(HookId h) noexcept {
    switch (h) {
        case HookId::ClientTick:       return "ClientTick";
        case HookId::GameLoop:         return "GameLoop";
        case HookId::KeyPress:         return "KeyPress";
        case HookId::MouseButton:      return "MouseButton";
        case HookId::PacketSend:       return "PacketSend";
        case HookId::PacketReceive:    return "PacketReceive";
        case HookId::RenderWorldPre:   return "RenderWorldPre";
        case HookId::RenderWorldPost:  return "RenderWorldPost";
        case HookId::RenderHud:        return "RenderHud";
        case HookId::EntityRender:     return "EntityRender";
        case HookId::PlayerMove:       return "PlayerMove";
        case HookId::Jump:             return "Jump";
        case HookId::Attack:           return "Attack";
        case HookId::UseItem:          return "UseItem";
        case HookId::Damage:           return "Damage";
        case HookId::Chat:             return "Chat";
        case HookId::ScreenOpen:       return "ScreenOpen";
        case HookId::ClickBlock:       return "ClickBlock";
        case HookId::GetFov:           return "GetFov";
        case HookId::EyeHeight:        return "EyeHeight";
        case HookId::BlockOutline:     return "BlockOutline";
        default:                       return "?";
    }
}

enum class HookLoad : std::uint8_t {
    Installed,   // patched at ClassFileLoadHook time; always active
    Lazy,        // patched on first arm() call by a subscribing module
};

} // namespace dt::hook
