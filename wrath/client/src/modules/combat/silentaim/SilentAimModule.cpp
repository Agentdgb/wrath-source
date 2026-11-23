// SilentAimModule.cpp — ClickGui glue for SilentAim.
//
// The controller runs from GameLoopEvent, which is pumped from the
// wglSwapBuffers detour — in MC 1.21 that thread is the same OS thread that
// runs the game loop, so the work lands on the game thread with no bytecode
// retransform on the pump path at all.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "util/Log.h"
#include "vgui/bridge.h"   // isOverlayVisible — stand down while the panel is up

#include "silentaim.h"
#include "silentaim_tick.h"

namespace vanta::modules {

class SilentAim : public Module {
public:
    SilentAim() : Module("Rotation Assist",
                         "Aims on the server, not on your screen",
                         Category::Combat) {
        registerSetting(&targets_);
        registerSetting(&fov_);
        registerSetting(&shieldCheck_);
        registerSetting(&onlyWeapon_);
        registerSetting(&extraReach_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){
            mirror();
            ::vanta::modules::silentaim::ensure_installed();
            ::vanta::modules::silentaim_tick::pump(e.deltaMs);
        });
    }

    void onEnable() override {
        ::vanta::modules::silentaim::settings::enabled.store(true, std::memory_order_relaxed);
        mirror();

        // The camera is never touched, so a missing hook cannot be felt — it
        // just means the rotation stops reaching the server. Say so, or the
        // module would look like it works while landing nothing.
        if (!::vanta::modules::silentaim::silent_rotation_available()) {
            VLOG_WARN("silentaim",
                      "the sendMovementPackets hook is not verified on this host — the "
                      "rotation will NOT reach the server. Hit redirection and the F5 "
                      "head still work. See the probe line at startup.");
        }
    }

    void onDisable() override {
        ::vanta::modules::silentaim::settings::enabled.store(false, std::memory_order_relaxed);
        // Do NOT reset() here: this runs on the render thread, and the reset
        // path touches JNI plus the triggerbot's target ref. The pump sees
        // enabled=false on its next step and tears down on the game thread.
    }

private:
    // Render thread writes, game thread reads — hence the atomic mirror.
    void mirror() {
        using S = ::vanta::modules::silentaim::settings;

        S::targets.store(targets_.index(),                std::memory_order_relaxed);
        S::max_angle.store(fov_.value(),                  std::memory_order_relaxed);
        S::shield_check.store(shieldCheck_.value(),       std::memory_order_relaxed);
        S::only_weapon.store(onlyWeapon_.value(),         std::memory_order_relaxed);
        S::extra_reach_x10.store(static_cast<int>(extraReach_.value() * 10.f),
                                 std::memory_order_relaxed);

        S::overlay_open.store(::vanta::vgui::bridge::isOverlayVisible(),
                              std::memory_order_relaxed);
    }

    // What the aura may lock onto.
    EnumSetting targets_    {this, "Targets", {"Players", "Mobs", "All"}, 0};

    // Vape's "Max angle" (default 120), halved before use exactly as
    // SilentAura.isValidTarget does — so 120 means 60 degrees off your cursor
    // in every direction.
    // Capped at 180 (90 degrees each way) rather than Vape's 360. Past that the
    // aura points behind you, which no server-side rotation model finds
    // plausible, and it walks the aim toward the +/-180 seam that the yaw fold
    // in update_aim has to stay clear of.
    IntSetting  fov_        {this, "FOV", 120, 1, 180};

    BoolSetting shieldCheck_{this, "Shield Check", false};

    // Only engage while holding a sword, axe, mace or trident.
    BoolSetting onlyWeapon_ {this, "Only Weapon",  false};



    // Vape's "Extra swing distance": how far PAST the 3.0 base reach the aura
    // will engage a target before attacking becomes possible.
    FloatSetting extraReach_{this, "Extra Reach", 0.0f, 0.0f, 3.0f};
};

VANTA_REGISTER_MODULE(SilentAim);

} // namespace vanta::modules
