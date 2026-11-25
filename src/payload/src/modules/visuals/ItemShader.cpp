// First-person held-item glow, ported from the reference project.
// The local observer replaces the reference Animations dependency.
#include "core/event/Events.h"
#include "core/module/Module.h"

#include "modules/render/chams/item_shader_gl.h"
#include "modules/render/chams/item_shader_hook.h"

namespace dt::modules {

namespace cgl = ::dt::modules::itemshader;

class ItemShader : public Module {
public:
    ItemShader() : Module("ItemShader",
                          "Soft colour-matched glow on your first-person held item.",
                          Category::Visuals) {
        // No knobs at all, by request — the look ships tuned by default and
        // the desktop diagnostics stay off (see cgl::set_debug below).
    }

    ~ItemShader() override { cgl::shutdown_hook(); }

    void onInit() override {
        // The GL layer is armed by chams::tick_game_context() (called every frame
        // from gl_hook), which ORs in item_shader_wanted() — so this works with
        // the Chams module OFF. The effect simply follows the module on/off.
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            const bool on = isEnabled() && cgl::install_hook();
            cgl::set_wanted(on);
            cgl::set_effect(on);
            cgl::set_debug(false);
        });
    }

    void onEnable()  override { cgl::set_wanted(true); cgl::set_effect(true); }
    void onDisable() override {
        cgl::set_wanted(false);
        cgl::set_effect(false);
        // Never leave the per-frame flag stuck raised if disabled mid-frame.
        cgl::set_rendering_firstperson(false);
    }

};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::ItemShader)
