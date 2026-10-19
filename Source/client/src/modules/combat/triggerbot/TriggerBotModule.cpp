// TriggerBotModule.cpp — Vanta ClickGui glue for the ported triggerbot.
//
// The ported triggerbot (triggerbot.cpp / triggerbot_hook.cpp) is engine-
// agnostic: it reads user preferences out of `settings` and expects
// `run()` to be called on the game/render thread. This module wires those
// two ends into Vanta:
//
//   • Registers a real vanta::Module (Category::Combat) so it shows up in
//     the ClickGui with every user-facing toggle the port supports.
//   • Every frame (GameLoopEvent) it mirrors its own Setting values into
//     `vanta::modules::triggerbot::settings::*` and calls `run()`, which
//     installs the JNIHook on first enable and drives everything from
//     there.
//   • On disable it flips `settings::enabled = false` and calls `run()`
//     once so the hook layer sees the drop and releases its focus /
//     last-target refs.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "util/Log.h"
#include "vgui/bridge.h"   // isOverlayVisible — stand down while the panel is up

#include "triggerbot.h"

// Forward decl of the desktop-.txt logger defined in triggerbot_hook.cpp.
// Used to trace ClickGui-driven onEnable/onDisable so we can tell whether
// the user's toggle is actually reaching this module.
extern "C" void vanta_trigger_desktop_log(const char* fmt, ...);

namespace vanta::modules {

class TriggerBot : public Module {
public:
    TriggerBot() : Module("TriggerBot",
                          "Hits whatever your crosshair lands on",
                          Category::Combat) {
        vanta_trigger_desktop_log("MOD: TriggerBot ctor — module registered under name=\"TriggerBot\" cat=Combat");
        registerSetting(&range_);
        registerSetting(&cooldownThreshold_);
        registerSetting(&randomizeCooldown_);
        registerSetting(&waitForCrit_);
        registerSetting(&checkShield_);
        registerSetting(&onlyWeapons_);
        registerSetting(&workInScreen_);
        registerSetting(&targetType_);
        registerSetting(&missChance_);
        registerSetting(&focusMode_);
        registerSetting(&showFocusVisuals_);
    }

    void onInit() override {
        vanta_trigger_desktop_log("MOD: TriggerBot::onInit — subscribing GameLoopEvent listener");
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            mirror();
            ::vanta::modules::triggerbot::run();
        });
    }

    void onEnable() override {
        vanta_trigger_desktop_log(
            "MOD: TriggerBot::onEnable CALLED — ClickGui toggle reached the module. "
            "Setting settings::enabled=true.");
        ::vanta::modules::triggerbot::settings::enabled = true;
        mirror();
    }

    void onDisable() override {
        vanta_trigger_desktop_log("MOD: TriggerBot::onDisable CALLED — settings::enabled=false");
        ::vanta::modules::triggerbot::settings::enabled = false;
        ::vanta::modules::triggerbot::run();
    }

private:
    // Mirror the Setting objects into the port's settings struct. All
    // reads happen on the same thread (game/render) so no atomics needed.
    void mirror() {
        using S = ::vanta::modules::triggerbot::settings;

        // The port carried `show_gui` over from enhance, where the ClickGui
        // toggle wrote it and every module checked it — but nothing in Vanta
        // ever assigned it, leaving the guard permanently false. The overlay
        // is an ImGui surface, not a Minecraft Screen, so the currentScreen
        // check in gate_attack_in_gui never caught it either: the bot kept
        // firing SendInput clicks straight into the open panel, toggling
        // modules and dragging sliders. Wire it to the real overlay state.
        S::show_gui           = ::vanta::vgui::bridge::isOverlayVisible();

        S::attack_range       = range_.value();
        S::cooldown_threshold = cooldownThreshold_.value();
        S::randomize_cooldown = randomizeCooldown_.value();
        S::wait_for_crit      = waitForCrit_.value();
        S::require_sprint     = false; // removed from GUI; keep gate disabled
        S::check_shield       = checkShield_.value();
        S::only_weapons       = onlyWeapons_.value();
        S::work_in_screen     = workInScreen_.value();
        S::target_type        = targetType_.index();
        S::miss_chance        = static_cast<int>(missChance_.value());
        S::focus_mode         = focusMode_.value();
        S::show_focus_visuals = showFocusVisuals_.value();
    }

    // Range default 3.0 = vanilla survival attack reach. When set above
    // 3.0, triggerbot_hook runs its own entity raycast past the vanilla
    // crosshair pick (Minecraft's own EntityHitResult caps at ≈3), so the
    // slider actually reaches the target it picks. Above 4.5 the server
    // usually starts rejecting hits — that ceiling is per-server.
    FloatSetting range_             {this, "Range",             3.0f,  1.0f, 6.0f, 0.05f};
    FloatSetting cooldownThreshold_ {this, "Cooldown",          90.0f, 0.0f, 100.0f, 1.0f};
    BoolSetting  randomizeCooldown_ {this, "RandomizeCooldown", true };
    // Won't attack while in the air unless the player is descending -- the
    // client-side vertical-motion test Vape uses ("player.getY() - prevY"),
    // which is exactly what the server evaluates for isCrit. Named
    // "WaitForCrit" instead of Vape's "Air crits" because that reads better.
    BoolSetting  waitForCrit_       {this, "WaitForCrit",       false};
    BoolSetting  checkShield_       {this, "CheckShield",       true };
    BoolSetting  onlyWeapons_       {this, "OnlyWeapons",       false};
    BoolSetting  workInScreen_      {this, "WorkInScreens",     false};
    EnumSetting  targetType_        {this, "Targets", {"All", "Players", "Mobs"}, 0};
    FloatSetting missChance_        {this, "MissChance",        0.0f,  0.0f, 75.0f, 1.0f};
    BoolSetting  focusMode_         {this, "FocusMode",         false};
    BoolSetting  showFocusVisuals_  {this, "ShowFocusHighlight", false};
};

VANTA_REGISTER_MODULE(TriggerBot);

} // namespace vanta::modules
