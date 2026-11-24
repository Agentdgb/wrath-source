#include "core/module/Module.h"
#include "aimassist/simple/Engine.h"
#include "aimassist/wind/Engine.h"
#include "modules/render/targetesp/target_attack.h"
#include "antibot/antibot.h"
#include "vgui/bridge.h"
#include "util/Log.h"
#include "util/XorStr.h"
#include <atomic>
#include <chrono>

namespace vanta::modules {
class MotionTuner final : public Module {
    aimassist::simple::Engine simpleEngine;
    aimassist::wind::Engine windEngine;
    std::atomic<const char *> state{"Disabled"};
    std::string lastError;
    EnumSetting mode{this, "Aim mode", {"Wind", "Simple"}, 1};
    FloatSetting smoothing{this, "Tracking smoothing", 52.f, 0.f, 100.f, 1.f};
    FloatSetting speed1{this, "Speed 1", 18.f, 1.f, 100.f, 1.f};
    FloatSetting windHorizontalSpeed{this, "Horizontal speed", 15.f, 1.f, 30.f, 1.f};
    FloatSetting windVerticalSpeed{this, "Vertical speed", 10.f, 1.f, 30.f, .1f};
    FloatSetting distance{this, "Distance", 3.5f, 1.f, 10.f, .1f};
    EnumSetting targets{this, "Simple targets", {"Players", "Mobs", "Animals", "All"}, 0};
    EnumSetting targetPosition{this, "Target point", {"Dynamic", "Center"}, 0};
    BoolSetting onlyWeapon{this, "Only weapon", true};
    BoolSetting vertical{this, "Vertical aim", true};
    FloatSetting fov{this, "Fov", 100.f, 10.f, 360.f, .1f};
    BoolSetting focusMode{this, "Focus mode", true};
    BoolSetting pauseOnEat{this, "Pause on eat", true};
    BoolSetting teams{this, "Teams", true};
    BoolSetting stopOnHit{this, "Stop on hit", false};
    FloatSetting stopOnHitDelay{this, "Hit pause delay", 400.f, 50.f, 1500.f, 25.f};
    BoolSetting onlyOnHit{this, "Only on hit", true};
    std::atomic<uint64_t> lastAttackTimeMs{0};
    std::atomic<uint64_t> lastMovingTimeMs{0};

    BoolSetting windVertical{this, "Wind vertical aim", true};
    BoolSetting windCheckBlock{this, "Check block break", false};
    FloatSetting windAngle{this, "Max angle", 180.f, 1.f, 360.f, 1.f};
    FloatSetting windDistance{this, "Wind distance", 3.5f, 1.f, 10.f, .1f};
    EnumSetting windArea{this, "Target area", {"Head", "Chest", "Legs"}, 1};
    BoolSetting windPlayers{this, "Wind players", true}, windMobs{this, "Wind mobs", false};
    BoolSetting windIgnoreInvisible{this, "Ignore invisible", false};
    BoolSetting windOnlyWeapons{this, "Only weapons", false};
    BoolSetting windTeams{this, "Wind teams", true};
    BoolSetting windFocus{this, "Wind focus mode", false};
    FloatSetting windFocusRange{this, "Focus range", 3.5f, 1.f, 10.f, .5f};

    readableaim::Settings settings() const {
        readableaim::Settings result;
        result.mode = static_cast<readableaim::AimMode>(mode.index());
        result.position = targetPosition.index() == 0 ? readableaim::AimPosition::Random : readableaim::AimPosition::Center;
        result.prediction = 0.045;
        result.focusMode = focusMode.value() ? readableaim::FocusMode::Auto : readableaim::FocusMode::Off;
        result.smoothing = smoothing.value();
        result.speed1 = speed1.value();
        result.distance = distance.value();
        result.maxFocusDistance = distance.value();
        result.clampDistance = true;
        result.fov = fov.value();
        result.players = targets.index() == 0 || targets.index() == 3;
        result.mobs = targets.index() == 1 || targets.index() == 3;
        result.animals = targets.index() == 2 || targets.index() == 3;
        result.onlyWeapon = onlyWeapon.value();
        result.vertical = vertical.value();
        result.pauseOnEat = pauseOnEat.value();
        result.teams = teams.value();
        result.stopOnHit = stopOnHit.value();
        result.stopOnHitMs = stopOnHitDelay.value();
        result.onlyOnHit = onlyOnHit.value() || focusMode.value();
        result.sanitize();
        return result;
    }
    aimassist::SettingsView windSettings() const {
        aimassist::SettingsView sv;
        sv.mode = 0;
        sv.requireMouseDown = false;
        sv.aimVertically = windVertical.value();
        sv.checkBlockBreak = windCheckBlock.value();
        sv.horizontalSpeed = windHorizontalSpeed.value();
        sv.verticalSpeed = windVerticalSpeed.value();
        sv.maxAngle = static_cast<int>(windAngle.value());
        sv.distance = windDistance.value();
        sv.targetMode = 0;
        sv.targetArea = windArea.index();
        sv.players = windPlayers.value();
        sv.mobs = windMobs.value();
        sv.ignoreInvisible = windIgnoreInvisible.value();
        sv.limitToItems = windOnlyWeapons.value();
        sv.teams = windTeams.value();
        sv.focusMode = windFocus.value();
        sv.focusRange = windDistance.value();
        sv.emissionSmooth = 5;
        sv.jumpCompensate = false;
        sv.humanize = false;
        return sv;
    }
    void publishState() {
        state.store(simpleEngine.status());
    }
    void frame() {
        if (!isEnabled())
            return;
        if (!antibot::allows_combat()) {
            windEngine.pause();
            simpleEngine.pause("Waiting for game input");
            publishState();
            return;
        }
        if (!native::focused()) {
            windEngine.pause();
            simpleEngine.pause("Game unfocused");
            publishState();
            return;
        }
        if (vgui::bridge::isEnabled("Freecam") || (mode.index() == 1 && vgui::bridge::isOverlayVisible())) {
            windEngine.pause();
            simpleEngine.pause("Client menu or Freecam");
            publishState();
            return;
        }
        if (pauseOnEat.value()) {
            const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (rightDown && !leftDown) {
                windEngine.pause();
                simpleEngine.pause("Eating gap / food");
                publishState();
                return;
            }
        }
        if (stopOnHit.value()) {
            const uint64_t nowMs = GetTickCount64();
            const uint64_t lastHit = lastAttackTimeMs.load();
            const uint64_t delay = static_cast<uint64_t>(stopOnHitDelay.value());
            if (lastHit > 0 && (nowMs - lastHit) < delay) {
                windEngine.pause();
                simpleEngine.pause("Paused on hit");
                publishState();
                return;
            }
        }
        jvm::JniScope scope;
        if (!scope) {
            windEngine.pause();
            simpleEngine.pause("Game unavailable");
            publishState();
            return;
        }
        if (mode.index() == 0) {
            simpleEngine.pause("Wind active");
            windEngine.step(scope.get(), windSettings());
            state.store(windEngine.status());
            return;
        }
        windEngine.pause();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (focusMode.value() || onlyOnHit.value()) targetesp::attack::install(scope.get());
        const uint64_t nowMs = GetTickCount64();
        const bool moving = (GetAsyncKeyState('W') & 0x8000) != 0 ||
                            (GetAsyncKeyState('A') & 0x8000) != 0 ||
                            (GetAsyncKeyState('S') & 0x8000) != 0 ||
                            (GetAsyncKeyState('D') & 0x8000) != 0 ||
                            (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 ||
                            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (moving) {
            lastMovingTimeMs.store(nowMs);
        }
        if (focusMode.value() || onlyOnHit.value()) {
            const uint64_t lastMove = lastMovingTimeMs.load();
            if (lastMove > 0 && (nowMs - lastMove) >= 20000) {
                simpleEngine.releaseTarget(scope.get());
            }
        }
        simpleEngine.step(scope.get(), settings(), now);
        publishState();
        if (simpleEngine.error() != lastError) {
            lastError = simpleEngine.error();
            if (!lastError.empty())
                VLOG_WARN("aimassist", "{}", lastError);
        }
    }

  public:
    MotionTuner()
        : Module(XOR_STR("AimAssist"), XOR_STR("Readable Aim Fabric controller port with manual mouse priority"), Category::Combat) {
        for (Setting *setting : std::initializer_list<Setting *>{
                 &mode, &windVertical, &windCheckBlock,
                 &windHorizontalSpeed, &windVerticalSpeed, &windAngle, &windDistance,
                 &windArea, &windPlayers, &windMobs, &windIgnoreInvisible,
                 &windOnlyWeapons, &windTeams, &windFocus, &windFocusRange,
                 &speed1, &smoothing, &distance, &fov, &vertical, &targets, &onlyWeapon, &teams, &focusMode, &pauseOnEat,
                 &stopOnHit, &stopOnHitDelay, &onlyOnHit})
            registerSetting(setting);
        windHorizontalSpeed.setVisibleIf([this] { return mode.index() == 0; });
        windVerticalSpeed.setVisibleIf([this] { return mode.index() == 0; });
        for (Setting *setting : std::initializer_list<Setting *>{
                 &speed1, &smoothing, &distance, &fov, &vertical, &targets, &onlyWeapon, &teams, &focusMode, &pauseOnEat,
                 &stopOnHit, &stopOnHitDelay, &onlyOnHit})
            setting->setVisibleIf([this] { return mode.index() == 1; });
        stopOnHitDelay.setVisibleIf([this] { return mode.index() == 1 && stopOnHit.value(); });
        for (Setting *setting : std::initializer_list<Setting *>{
                 &windVertical, &windCheckBlock, &windAngle, &windDistance,
                 &windArea, &windPlayers, &windMobs, &windIgnoreInvisible, &windOnlyWeapons, &windTeams, &windFocus})
            setting->setVisibleIf([this] { return mode.index() == 0; });
        windFocusRange.setVisibleIf([this] { return mode.index() == 0; });
        windCheckBlock.setDescription("Wind only: pauses assistance while attacking a block");
        windIgnoreInvisible.setDescription("Wind only: exclude invisible targets");
        windFocus.setDescription("Wind only: retain the same rival while valid within Focus range");
        windVertical.setLabel("Vertical aim");
        windDistance.setLabel("Distance");
        windPlayers.setLabel("Players");
        windMobs.setLabel("Mobs");
        windTeams.setLabel("Teams");
        windTeams.setDescription("Wind only: auto-detecta y no asiste contra companeros de equipo (Scoreboard, prefijos y color de nametag).");
        windFocus.setLabel("Focus mode");
        mode.setDescription(
            "Simple provides smooth tracking for the selected targets. Wind uses the original Vanta Regular controller");
        speed1.setLabel("Fuerza");
        speed1.setDescription("How strongly Simple follows the selected target. Higher values pull faster.");
        smoothing.setLabel("Suavidad");
        smoothing.setDescription("Softens starts, braking and direction changes. 50 balances smoothness and responsive tracking.");
        distance.setLabel("Alcance");
        distance.setDescription("Distancia exacta en bloques para asistir la mira.");
        fov.setLabel("Angulo");
        fov.setDescription("Total cone width in degrees around the crosshair. 100 allows up to 50 degrees on either side.");
        vertical.setDescription("Suave asistencia vertical que acompana los saltos y desniveles sin forzar la mira.");
        focusMode.setLabel("Focus");
        focusMode.setDescription("Keep following the same target until it dies or you attack another.");
        pauseOnEat.setLabel("Pausar al comer");
        pauseOnEat.setDescription("Pausa la asistencia mientras se come comida, gapple o se usa un consumible.");
        vertical.setLabel("Vertical");
        targets.setLabel("Objetivos");
        targets.setDescription("Choose players, mobs, animals, or all three. Focus uses the same target filter.");
        onlyWeapon.setLabel("Solo armas");
        onlyWeapon.setDescription("When enabled, assistance requires a sword, axe, mace or trident in the main hand.");
        teams.setLabel("Teams");
        teams.setDescription("Auto-detecta y no asiste contra companeros de equipo (Scoreboard, prefijos y color de nametag).");
        stopOnHit.setLabel("Stop on hit");
        stopOnHit.setDescription("Pausa la asistencia en cuanto le pegas a alguien para no forzar la mira durante el retroceso.");
        stopOnHitDelay.setLabel("Hit pause delay");
        stopOnHitDelay.setDescription("Milisegundos de pausa tras conectar un golpe antes de reanudar el tracking.");
        onlyOnHit.setLabel("Only on hit");
        onlyOnHit.setDescription("Solo asiste contra objetivos a los que ya les hayas pegado (no apunta a nadie antes del primer golpe).");
    }
    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent &) { frame(); });
        listen<events::AttackExecutedEvent>([this](events::AttackExecutedEvent &event) {
            const uint64_t now = GetTickCount64();
            lastAttackTimeMs.store(now);
            lastMovingTimeMs.store(now);
            if (isEnabled()) {
                if (stopOnHit.value() && !focusMode.value() && !onlyOnHit.value()) {
                    simpleEngine.releaseTarget(event.env);
                }
                if (mode.index() == 1 && (focusMode.value() || onlyOnHit.value()))
                    simpleEngine.focusAttack(event.env, event.target, settings());
            }
        });
    }
    void onEnable() override {
        lastAttackTimeMs.store(0);
        lastMovingTimeMs.store(GetTickCount64());
        jvm::JniScope scope;
        if (scope && (focusMode.value() || onlyOnHit.value()))
            targetesp::attack::install(scope.get());
        simpleEngine.pause(onlyOnHit.value() ? "Waiting for hit" : "Searching");
        publishState();
    }
    void onDisable() override {
        windEngine.pause();
        jvm::JniScope scope;
        if (scope)
            simpleEngine.clear(scope.get());
        simpleEngine.pause("Disabled");
        publishState();
        lastError.clear();
    }
    std::string displayInfo() const override { return state.load(); }
};
} // namespace vanta::modules
VANTA_REGISTER_MODULE(vanta::modules::MotionTuner)
