#include "Bloom.h"
#include "core/module/Module.h"
#include "modules/visuals/ports/VisualPorts.h"
namespace dt::modules::bloom {
class WorldParticles final : public Module {
    BoolSetting stars_{this, "Stars", true}, spark_{this, "Spark", false}, flame_{this, "Flame", false},
        runes_{this, "Runes", false}, snow_{this, "Snow", false}, theme_{this, "Theme Color", true};
    FloatSetting density_{this, "Density", 35, 0, 100, 1};
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
} // namespace dt::modules::bloom
DT_REGISTER_MODULE(dt::modules::bloom::WorldParticles)
