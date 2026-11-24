#include "core/keybind/KeybindManager.h"

#include "core/event/EventBus.h"
#include "core/module/ModuleManager.h"
#include "core/setting/KeybindSetting.h"
#include "vgui/vgui.h"
#include "util/Log.h"
#include "jvm/JniHelper.h"

#include <windows.h>

#include <array>
#include <memory>

namespace vanta {

struct KeybindManager::Impl {};

KeybindManager& KeybindManager::get() {
    static KeybindManager g;
    return g;
}

void KeybindManager::init() {
    if (!impl_) impl_ = new Impl();
    edges_ = {};
    // One authoritative input route avoids toggling twice if a Java key hook
    // and the Win32 state both observe the same press.
    VLOG_INFO("bind", "KeybindManager initialized (GUI=INSERT, PANIC=F8)");
}

void KeybindManager::shutdown() {
    if (!impl_) return;
    delete impl_;
    impl_ = nullptr;
}

void KeybindManager::onKey(events::KeyPressEvent& e) {
    if (e.repeat || e.virtualKey <= 0 || e.virtualKey >= 256) return;
    if (!e.down) {
        for (auto& m : ModuleManager::get().all())
            if (m->keybind().vk() == e.virtualKey && m->keybind().mode() == BindMode::Hold)
                m->setEnabled(false);
        return;
    }

    if (e.virtualKey == guiToggle_ && onGuiToggle_) {
        onGuiToggle_();
        e.handled = true;
        return;
    }
    if (panicKey_ > 0 && e.virtualKey == panicKey_) {
        ModuleManager::get().panic();
        e.handled = true;
        return;
    }

    for (auto& m : ModuleManager::get().all()) {
        /* Unbound is the default for every module in the registry, so without
           this a KeyPressEvent carrying virtualKey 0 -- which is what an
           unmapped key or a cleared bind produces -- would match all of them at
           once and toggle the entire client. */
        if (m->keybind().vk() == 0) continue;

        if (m->keybind().vk() != e.virtualKey) continue;
        switch (m->keybind().mode()) {
            case BindMode::Toggle: m->toggle(); break;
            case BindMode::Once:   m->setEnabled(true); if(!m->deferredClick()) clickPending_.emplace_back(m->name()); break;
            case BindMode::Hold:   m->setEnabled(true); break;
            case BindMode::Disabled: break;
            case BindMode::Always: break;
        }
    }
}

void KeybindManager::dispatchQueuedBinds() {
    if (!impl_) return;
    for(const auto& name:clickPending_) if(auto* m=ModuleManager::get().find(name))
        if(m->keybind().mode()==BindMode::Once) m->setEnabled(false);
    clickPending_.clear();

    std::vector<unsigned> bound_vks;
    bound_vks.reserve(16);
    std::array<bool,256> wanted{};
    for (const auto& m : ModuleManager::get().all()) {
        if (m->keybind().mode() == BindMode::Always) {
            m->setEnabled(true);
        }
        int vk = m->keybind().vk();
        if (vk > 0 && vk < 256 && vk != guiToggle_ && vk != panicKey_) {
            unsigned uvk = static_cast<unsigned>(vk);
            if (!wanted[uvk]) {
                wanted[uvk] = true;
                bound_vks.push_back(uvk);
            }
        }
    }

    if (bound_vks.empty()) return;

    bool any_down = false;
    std::array<bool,256> down_state{};
    for (unsigned vk : bound_vks) {
        const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        down_state[vk] = down;
        if (down) any_down = true;
    }

    bool allowed = !(vgui::isRunning() && vgui::isVisible());
    if (allowed && any_down) {
        DWORD pid = 0;
        HWND foreground = GetForegroundWindow();
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        allowed = (pid == GetCurrentProcessId());
        if (allowed) {
            jvm::JniScope scope;
            if (scope) {
                auto mc = jvm::staticCallObjectCanon(scope.get(), "MinecraftClient", "getInstance");
                if (mc) {
                    jvm::LocalObject screen(scope.get(), jvm::getObjectFieldCanon(
                        scope.get(), mc.get(), "MinecraftClient", "currentScreen"));
                    if (screen) allowed = false;
                }
            }
        }
    }

    for (unsigned vk : bound_vks) {
        int edge = edges_.update(vk, down_state[vk], allowed);
        if (!edge) continue;
        events::KeyPressEvent event{{},static_cast<int>(vk),edge>0,false,false};
        onKey(event);
    }
}

} // namespace vanta
