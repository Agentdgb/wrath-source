#include "SwordReplace.h"
#include "core/module/Module.h"

namespace vanta::modules::swordreplace {
class SwordReplace final : public Module {
    EnumSetting model_{this, "Model", models, std::size(models), defaultModel};
public:
    SwordReplace() : Module("SwordReplace", "Sword cosmetics - 20 models", Category::Visuals) {
        registerSetting(&model_);
        listen<events::GameLoopEvent>([this](auto&) {
            update(isEnabled(), model_.index());
            if (description() != status()) setDescription(status());
        });
    }
    // Activation may precede JVM bootstrap (including profile loading).
    // Keep the requested state; the frame listener finishes installation.
    void onDisable() override { update(false, model_.index()); }
};
}
VANTA_REGISTER_MODULE(vanta::modules::swordreplace::SwordReplace)
