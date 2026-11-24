// EspModule.cpp — Vanta module wrapper for the Player ESP engine.

#include "esp.h"

#include "core/module/Module.h"

namespace vanta::modules {

class Esp : public Module {
public:
    Esp()
        : Module("PlayerBoxes", "Boxes, health and gear on players",
                 Category::Visuals)
    {
        registerSetting(&mode_);
        registerSetting(&fill_);
        registerSetting(&health_);
        registerSetting(&equipment_);
        registerSetting(&showself_);
        registerSetting(&color_);
        registerSetting(&range_);
        registerSetting(&thickness_);
        thickness_.setVisibleIf([this] { return mode_.index() < 2; });
        push();

        // Fill doesn't apply to the Skeleton mode — hide it there.
        fill_.setVisibleIf([this] { return mode_.index() != 3; });

        // Live pull each frame — ClickGui edits go through the bridge, which
        // never calls our push(); the provider keeps g_settings current.
        esp::g_settings_provider = [this](esp::settings& s) {
            s.mode      = mode_.index();
            s.fill      = fill_.value();
            s.health    = health_.value();
            s.equipment = equipment_.value();
            s.showself  = showself_.value();
            const auto c = color_.value();
            s.col_r = c.r; s.col_g = c.g; s.col_b = c.b; s.col_a = c.a;
            s.range = range_.value();
        s.thickness = thickness_.value();
            };
    }
    ~Esp() override { esp::g_settings_provider = {}; }

    void onInit() override { esp::install(); }

private:
    EnumSetting  mode_     { this, "Mode", { "2D Box", "3D Box", "Wireframe", "Skeleton" }, 0 };
    BoolSetting  fill_     { this, "Fill",            false };
    BoolSetting  health_   { this, "Health",          true  };
    BoolSetting  equipment_{ this, "Show Equipment",  true  };
    BoolSetting  showself_ { this, "Show Self",       false };
    ColorSetting color_    { this, "Color", RGBA{ 1.0f, 1.0f, 1.0f, 1.0f } };
    // Match the GUI's one-pixel border: thin, crisp, and without a heavy halo.
    FloatSetting thickness_{ this, "Thickness", 1.0f, 0.5f, 2.0f, 0.05f };
    FloatSetting range_    { this, "Range", 64.0f, 8.0f, 128.0f, 1.0f };

    void push() {
        auto& s = esp::g_settings;
        s.mode      = mode_.index();
        s.fill      = fill_.value();
        s.health    = health_.value();
        s.equipment = equipment_.value();
        s.showself  = showself_.value();
        const auto c = color_.value();
        s.col_r = c.r; s.col_g = c.g; s.col_b = c.b; s.col_a = c.a;
        s.range = range_.value();
        s.thickness = thickness_.value();
    }

    void onEnable() override  { push(); esp::g_enabled = true; }
    void onDisable() override { esp::g_enabled = false; }

    void drawImGui() override { Module::drawImGui(); push(); }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::Esp)
