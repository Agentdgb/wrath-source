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
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "util/XorStr.h"
#include "vgui/bridge.h"   // isOverlayVisible — stand down while the panel is up

#include "triggerbot.h"
#include "triggerbot_hook.h"
#include "modules/render/targetesp/target_attack.h"

#define vanta_trigger_desktop_log(...) ((void)0)

namespace vanta::modules {

class ActionDispatch : public Module {
public:
    ActionDispatch() : Module(XOR_STR("TriggerBot"),
                              XOR_STR("Hits whatever your crosshair lands on"),
                              Category::Combat) {
        registerSetting(&cooldownThreshold_);
        registerSetting(&randomizeCooldown_);
        randomizeCooldown_.setLabel("Randomization");
        randomizeCooldown_.setDescription("Calcula dinamicamente la cadencia: criticos rapidos (>0.90F) al descender y barridos a carga completa (100%) en suelo con micro-variacion humana.");
        registerSetting(&waitForCrit_);
        waitForCrit_.setDescription("Prioriza golpes criticos al descender (al limite pero rapido).");
        registerSetting(&fullChargeNormal_);
        fullChargeNormal_.setDescription("Requiere carga completa en golpes terrestres para asegurar golpes limpios y normales.");
        registerSetting(&checkShield_);
        registerSetting(&checkTeams_);
        checkTeams_.setDescription("Auto-detecta y no ataca a companeros de equipo (Scoreboard, prefijos y color de nametag).");
        registerSetting(&maxHurtTime_);
        maxHurtTime_.setDescription("Max target hurtTime to allow attacking. 0 requires target to be completely vulnerable, avoiding wasted hits during damage immunity.");
        registerSetting(&onlyWeapons_);
        registerSetting(&workInScreen_);
        registerSetting(&targetType_);
        registerSetting(&missChance_);
        registerSetting(&focusMode_);
        focusMode_.setDescription("Keep attacking the same target until it dies or you attack another.");
        registerSetting(&onlyOnHit_);
        onlyOnHit_.setDescription("Solo ataca al objetivo despues de que le hayas pegado tu primero (requiere golpe manual para activar).");
        registerSetting(&postEatDelay_);
        postEatDelay_.setLabel("Pausa post-gapple");
        postEatDelay_.setDescription("Pausa en milisegundos tras comer gapple o comida antes de reanudar el ataque para evitar parecer que pegas mientras comes.");
        registerSetting(&range_);
        registerSetting(&showFocusVisuals_);
        focusStyle_.setVisibleIf([this] { return showFocusVisuals_.value(); });
        arrowStyle_.setVisibleIf([this] { return showFocusVisuals_.value() && focusArrow_.value(); });
        previewMode_.setVisibleIf([this] { return showFocusVisuals_.value(); });
        registerSetting(&focusStyle_);
        registerSetting(&arrowStyle_);
        registerSetting(&previewMode_);
        focusDuration_.setVisibleIf([this] { return showFocusVisuals_.value(); });
        focusDuration_.setDescription("Seconds visible after the last attack. 0 keeps it visible while focused.");
        focusArrow_.setVisibleIf([this] { return showFocusVisuals_.value(); });
        focusDistance_.setVisibleIf([this] { return showFocusVisuals_.value(); });
        registerSetting(&focusDuration_);
        registerSetting(&focusArrow_);
        registerSetting(&focusDistance_);
    }

    void onInit() override {
        vanta_trigger_desktop_log("MOD: TriggerBot::onInit — subscribing GameLoopEvent listener");
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            mirror();
            ::vanta::modules::triggerbot::run();
            if (isEnabled() && (focusMode_.value() || onlyOnHit_.value())) {
                jvm::JniScope scope;
                if (scope) targetesp::attack::install(scope.get());
            }
        });
        listen<events::AttackExecutedEvent>([](events::AttackExecutedEvent &event) {
            triggerbot_hook::on_entity_attacked(event.env, event.target);
        });
    }

    void onEnable() override {
        vanta_trigger_desktop_log(
            "MOD: TriggerBot::onEnable CALLED — ClickGui toggle reached the module. "
            "Setting settings::enabled=true.");
        ::vanta::modules::triggerbot::settings::enabled = true;
        mirror();
        if (focusMode_.value() || onlyOnHit_.value()) {
            jvm::JniScope scope;
            if (scope) targetesp::attack::install(scope.get());
        }
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
        S::full_charge_normal = fullChargeNormal_.value();
        S::check_shield       = checkShield_.value();
        S::check_teams        = checkTeams_.value();
        S::max_hurt_time      = static_cast<int>(maxHurtTime_.value());
        S::require_click      = false;
        S::only_weapons       = onlyWeapons_.value();
        S::work_in_screen     = workInScreen_.value();
        S::target_type        = targetType_.index();
        S::miss_chance        = static_cast<int>(missChance_.value());
        S::focus_mode         = focusMode_.value();
        S::only_on_hit        = onlyOnHit_.value();
        S::post_eat_delay_ms  = postEatDelay_.value();
        S::show_focus_visuals = showFocusVisuals_.value();
        S::focus_frame_style = focusStyle_.index();
        S::focus_arrow_style = arrowStyle_.index();
        S::focus_visual_seconds = focusDuration_.value();
        S::focus_arrow = focusArrow_.value();
        S::focus_distance = focusDistance_.value();
    }

    // Range default 3.0 = vanilla survival attack reach. When set above
    // 3.0, triggerbot_hook runs its own entity raycast past the vanilla
    // crosshair pick (Minecraft's own EntityHitResult caps at ≈3), so the
    // slider actually reaches the target it picks. Above 4.5 the server
    // usually starts rejecting hits — that ceiling is per-server.
    FloatSetting range_             {this, "Range",             3.0f,  1.0f, 10.0f, 0.1f};
    FloatSetting cooldownThreshold_ {this, "Cooldown",          95.0f, 0.0f, 100.0f, 1.0f};
    BoolSetting  randomizeCooldown_ {this, "RandomizeCooldown", true };
    // Won't attack while in the air unless the player is descending -- the
    // client-side vertical-motion test Vape uses ("player.getY() - prevY"),
    // which is exactly what the server evaluates for isCrit. Named
    // "WaitForCrit" instead of Vape's "Air crits" because that reads better.
    BoolSetting  waitForCrit_       {this, "WaitForCrit",       true };
    BoolSetting  fullChargeNormal_  {this, "FullChargeGround",  false };
    BoolSetting  checkShield_       {this, "CheckShield",       true };
    BoolSetting  checkTeams_        {this, "Teams",             true };
    FloatSetting maxHurtTime_       {this, "MaxHurtTime",       0.0f,  0.0f, 10.0f, 1.0f};
    BoolSetting  onlyWeapons_       {this, "OnlyWeapons",       false};
    BoolSetting  workInScreen_      {this, "WorkInScreens",     false};
    EnumSetting  targetType_        {this, "Targets", {"All", "Players", "Mobs"}, 0};
    FloatSetting missChance_        {this, "MissChance",        0.0f,  0.0f, 75.0f, 1.0f};
    BoolSetting  focusMode_         {this, "FocusMode",         true};
    BoolSetting  onlyOnHit_         {this, "Only on hit",       false};
    FloatSetting postEatDelay_      {this, "PostEatDelay",      100.0f, 0.0f, 500.0f, 10.0f};
    BoolSetting  showFocusVisuals_  {this, "ShowFocusHighlight", false};
    EnumSetting  focusStyle_        {this, "Focus Style", {"Corners", "Box", "Arrow Only"}, 0};
    EnumSetting  arrowStyle_        {this, "Arrow Style", {"Chevron", "Double", "Diamond"}, 0};
    BoolSetting  previewMode_       {this, "Preview Mode", false};
    FloatSetting focusDuration_     {this, "Highlight Time", 0.0f, 0.0f, 30.0f, 1.0f};
    BoolSetting  focusArrow_        {this, "Focus Arrow", true};
    BoolSetting  focusDistance_     {this, "Focus Distance", true};
};

VANTA_REGISTER_MODULE(ActionDispatch);

} // namespace vanta::modules
