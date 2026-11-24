#include "core/module/Module.h"
#include "modules/combat/shieldbreaker/ShieldBreaker.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include "jvm/JniHelper.h"
#include <sdk/mappings/mappings.hpp>

namespace vanta::modules {
class ShieldBreaker final : public Module {
public:
    ShieldBreaker() : Module("ShieldBreaker", "Switches visibly to a hotbar axe against raised shields", Category::Combat) {
        registerSetting(&onlySwords_);
        registerSetting(&switchBack_);
        registerSetting(&returnDelay_);
        registerSetting(&checkFacing_);
        registerSetting(&axeSelection_);
        returnDelay_.setVisibleIf([this] { return switchBack_.value(); });
        returnDelay_.setDescription("Milliseconds to keep the axe selected before restoring your previous slot.");
    }
    ~ShieldBreaker() override { shieldbreaker::set_enabled(false); }
    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& event) {
            mirror();
            if (!shieldbreaker::has_pending_return()) return;
            jvm::JniScope scope;
            if (!scope) return;
            auto* env = scope.get();
            const auto& cache = triggerbot::get_cache();
            if (!cache.minecraft_class) { shieldbreaker::clear_pending_return(env, false); return; }
            const auto instance = env->GetStaticFieldID(cache.minecraft_class,
                sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
            if (env->ExceptionCheck()) { env->ExceptionClear(); shieldbreaker::clear_pending_return(env, false); return; }
            if (!instance) { shieldbreaker::clear_pending_return(env, false); return; }
            jvm::LocalObject mc(env, env->GetStaticObjectField(cache.minecraft_class, instance));
            if (env->ExceptionCheck()) { env->ExceptionClear(); shieldbreaker::clear_pending_return(env, false); return; }
            shieldbreaker::pump_return(env, mc.get(), event.deltaMs);
        });
        listen<events::WorldChangeEvent>([](events::WorldChangeEvent& event) {
            shieldbreaker::clear_pending_return(event.env, false);
        });
    }
private:
    BoolSetting onlySwords_{this, "OnlySwords", true, "Swap only while holding a sword. An already selected axe stays selected."};
    BoolSetting switchBack_{this, "SwitchBack", true, "Restore your previous slot after the attack."};
    IntSetting returnDelay_{this, "ReturnDelay", 150, 50, 500};
    BoolSetting checkFacing_{this, "CheckFacing", true, "Swap only when the raised shield faces you."};
    EnumSetting axeSelection_{this, "AxeSelection", {"Strongest", "First in hotbar"}, 0};
    void mirror() {
        shieldbreaker::configure({onlySwords_.value(), switchBack_.value(), checkFacing_.value(),
            axeSelection_.index() == 0, returnDelay_.value()});
    }
    void onEnable() override { mirror(); shieldbreaker::set_enabled(true); }
    void onDisable() override { shieldbreaker::set_enabled(false); }
};
VANTA_REGISTER_MODULE(ShieldBreaker);
}
