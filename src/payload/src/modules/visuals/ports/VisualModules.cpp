#include "VisualPorts.h"
#include "core/module/Module.h"
namespace vanta::modules::visualports {
SwingConfig swing;
class SwingAnimation final : public Module {
    EnumSetting style_{this, "Style", {"Slash", "Chop", "Hook", "Spin", "Double Tap", "Push"}, 0};
    FloatSetting tempo_{this, "Tempo", 1, .4f, 2.5f, .05f}, intensity_{this, "Intensity", 1, .2f, 2, .05f};
    BoolSetting offhand_{this, "Offhand Too", true}, combat_{this, "Combat Only", false};

  public:
    SwingAnimation() : Module("SwingAnimation", "Custom first-person swing animations", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&style_, &tempo_, &intensity_, &offhand_, &combat_})
            registerSetting(s);
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            swing.enabled = isEnabled();
            swing.style = style_.index();
            swing.tempo = tempo_.value();
            swing.intensity = intensity_.value();
            swing.offhand = offhand_.value();
            swing.combatOnly = combat_.value();
            if (isEnabled())
                setDescription(swingStatus());
        });
    }
    void onDisable() override { swing.enabled = false; }
};
} // namespace vanta::modules::visualports
VANTA_REGISTER_MODULE(vanta::modules::visualports::SwingAnimation)
