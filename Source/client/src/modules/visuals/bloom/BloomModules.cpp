#include "Atmosphere.h"
#include "Bloom.h"
#include "DeathEffects.h"
#include "core/module/Module.h"
#include "modules/visuals/ports/VisualPorts.h"
namespace vanta::modules::bloom {
class DeathEffects final : public Module {
    EnumSetting style_{this, "Style", {"Soul", "Shockwave", "Shatter", "Lightning", "Supernova", "Void", "Random"}, 1},
        on_{this, "On", {"All", "Players", "Mobs"}, 0};
    BoolSetting theme_{this, "Theme Color", true}, hide_{this, "Hide Body", true}, sounds_{this, "Sounds", true},
        flash_{this, "Flash", false}, own_{this, "Own Death", false};
    FloatSetting scale_{this, "Scale", 1, .25f, 4, .05f}, amount_{this, "Particles", 1, 0, 2, .05f},
        max_{this, "Max Effects", 4, 1, 8, 1};
    ColorSetting color_{this, "Color", RGBA{.75f, .5f, 1, 1}};

  public:
    DeathEffects() : Module("DeathEffects", "Animated effects when an entity dies", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&style_, &on_, &scale_, &amount_, &max_, &theme_, &color_,
                                                        &hide_, &sounds_, &flash_, &own_})
            registerSetting(s);
        color_.setVisibleIf([this] { return !theme_.value(); });
        listen<events::GameLoopEvent>([this](auto &) {
            if (visualports::isStopping())
                return;
            auto c = color_.value();
            deathSettings = {isEnabled(),    theme_.value(),
                             hide_.value(),  sounds_.value(),
                             flash_.value(), own_.value(),
                             style_.index(), int(max_.value()),
                             on_.index(),    int(c.r * 255) << 16 | int(c.g * 255) << 8 | int(c.b * 255),
                             scale_.value(), amount_.value()};
            if (isEnabled())
                setDescription(hide_.value() && deathHideError() ? deathHideError() : runtimeStatus());
        });
    }
    void onEnable() override { setDeathInterest(true); }
    void onDisable() override {
        setDeathInterest(false);
        deathSettings.enabled = false;
    }
};
class WorldParticles final : public Module {
    BoolSetting stars_{this, "Stars", true}, spark_{this, "Spark", false}, flame_{this, "Flame", false},
        runes_{this, "Runes", false}, snow_{this, "Snow", false}, theme_{this, "Theme Color", true};
    FloatSetting density_{this, "Density", 100, 0, 100, 1};
    ColorSetting color_{this, "Particle Color", RGBA{.8f, .5f, 1, 1}};

  public:
    WorldParticles() : Module("WorldParticles", "Ambient particles around you", Category::Visuals) {
        for (auto *s :
             std::initializer_list<Setting *>{&stars_, &spark_, &flame_, &runes_, &snow_, &density_, &theme_, &color_})
            registerSetting(s);
        color_.setVisibleIf([this] { return !theme_.value(); });
        listen<events::GameLoopEvent>([this](auto &) {
            if (visualports::isStopping())
                return;
            worldSettings = {isEnabled(),   stars_.value(), spark_.value(),         flame_.value(), runes_.value(),
                             snow_.value(), theme_.value(), density_.value() / 100, color_.value()};
            if (isEnabled())
                setDescription(runtimeStatus());
        });
    }
    void onDisable() override { worldSettings.enabled = false; }
};
class FallingPetals final : public Module {
    EnumSetting density_{this, "Density", {"Normal", "Heavy"}, 0};

  public:
    FallingPetals() : Module("FallingPetals", "Sakura petals behind the client panels", Category::Visuals) {
        registerSetting(&density_);
        listen<events::GameLoopEvent>([this](auto &) {
            if (visualports::isStopping())
                return;
            petalSettings = {isEnabled(), density_.index() + 1};
        });
    }
    void onDisable() override { petalSettings.enabled = false; }
};
class Atmosphere final : public Module {
    EnumSetting mode_{this, "Sky Mode", {"Tint", "Solid"}, 0},
        color_{this, "Sky Color", {"Purple", "Blue", "Cyan", "Green", "Red", "Orange", "Pink", "Theme"}, 0};
    EnumSetting top_{this,
                     "Top Color",
                     {"Vanilla", "Match", "Night", "Purple", "Blue", "Cyan", "Green", "Red", "Orange", "Pink", "Theme"},
                     1};
    EnumSetting preset_{this,
                        "Sky Preset",
                        {"Blizzard", "Inferno", "Sakura", "Galaxy", "Aurora", "Autumn", "Fireflies", "Sandstorm",
                         "Blood Moon", "Synthwave", "Dusk", "Frost", "Neon", "None"},
                        13};
    EnumSetting fogMode_{this, "Fog Mode", {"Density", "Distance"}, 0};
    BoolSetting sky_{this, "Custom Sky", true}, fog_{this, "Fog", true}, rainbow_{this, "Rainbow", false},
        clouds_{this, "Cloud Tint", false}, time_{this, "Custom Time", false};
    FloatSetting strength_{this, "Strength", 100, 0, 100, 1}, cycle_{this, "Rainbow Cycle", 6, 1, 30, .5f},
        stars_{this, "Stars", .65f, 0, 1, .05f}, start_{this, "Fog Start", 12, 0, 256, 1},
        end_{this, "Fog End", 128, 1, 512, 1}, density_{this, "Fog Density", 0, 0, 100, 1},
        tint_{this, "World Tint", 0, 0, 100, 1}, ticks_{this, "Time", 6000, 0, 23999, 100},
        presetStrength_{this, "Preset Strength", 90, 0, 100, 1}, presetDensity_{this, "Preset Density", 55, 0, 100, 1},
        detail_{this, "Detail", 45, 5, 100, 1};

  public:
    Atmosphere() : Module("Atmosphere", "Native sky, stars and fog", Category::Visuals) {
        for (auto *setting : std::initializer_list<Setting *>{
                 &sky_,   &mode_,   &color_,          &top_,           &strength_, &rainbow_, &cycle_,   &clouds_,
                 &stars_, &preset_, &presetStrength_, &presetDensity_, &detail_,   &fog_,     &fogMode_, &density_,
                 &start_, &end_,    &tint_,           &time_,          &ticks_})
            registerSetting(setting);
        cycle_.setVisibleIf([this] { return rainbow_.value(); });
        start_.setVisibleIf([this] { return fog_.value() && fogMode_.index() == 1; });
        end_.setVisibleIf([this] { return fog_.value() && fogMode_.index() == 1; });
        density_.setVisibleIf([this] { return fog_.value() && fogMode_.index() == 0; });
        ticks_.setVisibleIf([this] { return time_.value(); });
        for (auto *setting : std::initializer_list<Setting *>{&presetStrength_, &presetDensity_, &detail_})
            setting->setVisibleIf([this] { return preset_.index() < 13; });
        listen<events::GameLoopEvent>([this](auto &e) {
            if (visualports::isStopping())
                return;
            atmosphere = {isEnabled(),
                          sky_.value(),
                          fog_.value(),
                          mode_.index(),
                          color_.index(),
                          stars_.value(),
                          start_.value(),
                          end_.value(),
                          rainbow_.value(),
                          clouds_.value(),
                          time_.value(),
                          top_.index(),
                          preset_.index(),
                          fogMode_.index(),
                          strength_.value() / 100,
                          cycle_.value(),
                          density_.value() / 100,
                          tint_.value() / 100,
                          ticks_.value(),
                          presetDensity_.value() / 100,
                          presetStrength_.value() / 100,
                          detail_.value() / 100};
            tickAtmosphere(e.deltaMs);
            if (isEnabled())
                setDescription(atmosphereStatus());
        });
    }
    void onDisable() override { atmosphere.enabled = false; }
};

} // namespace vanta::modules::bloom
VANTA_REGISTER_MODULE(vanta::modules::bloom::WorldParticles)
VANTA_REGISTER_MODULE(vanta::modules::bloom::FallingPetals)
VANTA_REGISTER_MODULE(vanta::modules::bloom::Atmosphere)

VANTA_REGISTER_MODULE(vanta::modules::bloom::DeathEffects)
