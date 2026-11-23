#include "antibot.h"
#include "activity_logic.h"
#include "core/module/Module.h"
#include <atomic>
#include <mutex>

namespace vanta::modules::antibot {
namespace {
std::atomic<bool> enabled{false};
std::atomic<unsigned> timeout_ms{300};
std::mutex activity_mx;
Activity activity;
unsigned movement_key(WPARAM key) {
    switch (key) {
    case 'W': return 1u; case 'A': return 2u; case 'S': return 4u; case 'D': return 8u;
    case VK_UP: return 16u; case VK_LEFT: return 32u; case VK_DOWN: return 64u;
    case VK_RIGHT: return 128u; case VK_SPACE: return 256u;
    default: return 0;
    }
}
void reset_activity() {
    std::lock_guard<std::mutex> lock(activity_mx);
    activity.reset();
}
}

bool allows_combat() {
    if (!enabled.load(std::memory_order_acquire)) return true;
    const auto foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    std::lock_guard<std::mutex> lock(activity_mx);
    return activity.active(GetTickCount64(), foreground, timeout_ms.load(std::memory_order_relaxed));
}

void observe_window_input(HWND window, UINT message, WPARAM wp, LPARAM lp, bool overlay_visible) {
    if (!enabled.load(std::memory_order_acquire)) return;
    if (overlay_visible || message == WM_KILLFOCUS || message == WM_DESTROY ||
        (message == WM_ACTIVATEAPP && !wp)) {
        reset_activity();
        return;
    }
    if (GetForegroundWindow() != window) return;
    const auto source_window = reinterpret_cast<std::uintptr_t>(window);
    const auto now = GetTickCount64();
    if (message == WM_INPUT) {
        // Observe Minecraft's existing raw-input stream; do not register devices
        // or consume the message, which would change the game's own mouse input.
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
        if (raw.header.dwType == RIM_TYPEMOUSE && raw.header.hDevice &&
            (raw.data.mouse.lLastX || raw.data.mouse.lLastY || raw.data.mouse.usButtonFlags)) {
            std::lock_guard<std::mutex> lock(activity_mx);
            activity.note(now, source_window, true);
        }
        return;
    }
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool mouse = message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN ||
        message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_MOUSEWHEEL;
    if (!key_down && !key_up && !mouse) return;
    INPUT_MESSAGE_SOURCE source{};
    // SendInput-generated trigger clicks and cursor warps must not keep us awake.
    if (!GetCurrentInputMessageSource(&source) || source.originId != IMO_HARDWARE) return;
    std::lock_guard<std::mutex> lock(activity_mx);
    if (key_down || key_up) activity.key(now, source_window, movement_key(wp), key_down, true);
    else activity.note(now, source_window, true);
}

class AntiBot : public Module {
public:
    AntiBot() : Module("AntiBot", "Pauses combat assistance while inactive", Category::Combat) {
        idleDelay_.setDescription("Segundos sin usar el raton o las teclas de movimiento antes de pausar la asistencia.");
        registerSetting(&idleDelay_);
    }
    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&) {
            timeout_ms.store(static_cast<unsigned>(idleDelay_.value() * 1000.f + 0.5f), std::memory_order_relaxed);
        });
    }
protected:
    void onEnable() override {
        reset_activity();
        timeout_ms.store(static_cast<unsigned>(idleDelay_.value() * 1000.f + 0.5f), std::memory_order_relaxed);
        enabled.store(true, std::memory_order_release);
    }
    void onDisable() override {
        enabled.store(false, std::memory_order_release);
        reset_activity();
    }
private:
    FloatSetting idleDelay_{this, "Inactividad (segundos)", 0.3f, 0.1f, 2.0f, 0.1f};
};
VANTA_REGISTER_MODULE(AntiBot);
}
