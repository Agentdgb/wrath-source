#pragma once
#include "core/event/Events.h"
#include "core/keybind/KeyEdges.h"
#include <functional>
#include <vector>
#include <string>
namespace vanta {
class KeybindManager {
public:
    static KeybindManager& get();
    void init();
    void shutdown();
    int guiToggleKey() const noexcept { return guiToggle_; }
    void setGuiToggleKey(int vk) noexcept { guiToggle_=vk; }
    int panicKey() const noexcept { return panicKey_; }
    void setPanicKey(int vk) noexcept { panicKey_=vk; }
    void setOnGuiToggle(std::function<void()> fn) { onGuiToggle_=std::move(fn); }
    // Poll and dispatch once on the game/render thread, before module updates.
    // The historical name remains for the render hook's public entry point.
    void dispatchQueuedBinds();
private:
    int guiToggle_=0x2D, panicKey_=0;
    std::function<void()> onGuiToggle_;
    struct Impl; Impl* impl_=nullptr;
    KeyEdges edges_;
    std::vector<std::string> clickPending_;
    void onKey(events::KeyPressEvent& e);
};
}
