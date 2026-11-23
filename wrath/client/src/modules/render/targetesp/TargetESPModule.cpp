#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "modules/visuals/bloom/Bloom.h"
#include "modules/visuals/ports/VisualPorts.h"
#include "target_attack.h"
#include "targetesp.h"
namespace vanta::modules::targetesp {
class TargetESP final : public Module {
    EnumSetting mode_{this, "Mode", {"Orbits", "Star", "Wings", "Sparkles", "Ring", "Sweep", "Bolts"}, 4};
    EnumSetting on_{this, "On", {"Target", "Self", "Both"}, 0};
    ColorSetting color_{this, "Color", RGBA{.78f, .49f, 1, .9f}};
    FloatSetting scale_{this, "Scale", 1, .25f, 3, .05f}, speed_{this, "Speed", 1.5f, .5f, 3, .1f},
        opacity_{this, "Opacity", .9f, .05f, 1, .05f};
    BoolSetting glow_{this, "Glow", true};

  public:
    TargetESP() : Module("TargetESP", "Around your target, in the world", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&mode_, &on_, &color_, &scale_, &opacity_, &speed_, &glow_})
            registerSetting(s);
        listen<events::GameLoopEvent>([this](auto &) {
            if (visualports::isStopping())
                return;
            bloom::targetSettings = {isEnabled(),    glow_.value(),    mode_.index(),  on_.index(),
                                     scale_.value(), opacity_.value(), speed_.value(), color_.value()};
            if (isEnabled())
                setDescription(bloom::runtimeStatus());
        });
    }
    void onEnable() override { attack::set_enabled(true); }
    void onDisable() override {
        bloom::targetSettings.enabled = false;
        attack::set_enabled(false);
        jvm::JniScope s;
        if (s)
            attack::reset(s.get());
    }
};
void render(int, int) {} // Submitted by the native world pass before the HUD.
} // namespace vanta::modules::targetesp
VANTA_REGISTER_MODULE(vanta::modules::targetesp::TargetESP)
