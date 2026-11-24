// HitFxModule.cpp — Vanta module wrapper for the comic hit-effects engine.

#include "hitfx.h"

#include "core/module/Module.h"

namespace vanta::modules {

class HitFx : public Module {
public:
    HitFx()
        : Module("HitFX", "Comic hit effects (POW/BAM) on hit players.", Category::Visuals)
    {
        registerSetting(&mode_);
        registerSetting(&glow_);
        registerSetting(&rainbow_);
        registerSetting(&color_);
        registerSetting(&scale_);
        registerSetting(&range_);
        push();

        // Live pull each frame — ClickGui edits go through the bridge, which
        // never calls our push(); the provider keeps g_settings current.
        hitfx::g_settings_provider = [this](hitfx::settings& s) {
            s.which = static_cast<hitfx::mode>(mode_.index());
            s.glow  = glow_.value();
            s.rainbow = rainbow_.value();
            const auto c = color_.value();
            s.col_r = c.r; s.col_g = c.g; s.col_b = c.b; s.col_a = c.a;
            s.scale = scale_.value();
            s.range = range_.value();
        };
    }
    ~HitFx() override { hitfx::g_settings_provider = {}; }

    // JNI-safe init point — install the dedicated matrix-capture hook here, not
    // in the ctor (no guaranteed JNIEnv at construction).
    void onInit() override {
        hitfx::install();
    }

private:
    EnumSetting  mode_   { this, "Mode", { "Comic", "Anime" }, 0 };
    BoolSetting  glow_   { this, "Glow", true };
    BoolSetting  rainbow_{ this, "Rainbow", false };
    ColorSetting color_  { this, "Color",   RGBA{ 1.0f, 0.80f, 0.05f, 1.0f } };
    FloatSetting scale_  { this, "Scale",   1.0f, 0.25f, 3.0f, 0.05f };
    FloatSetting range_  { this, "Range",   20.0f, 4.0f, 64.0f, 1.0f };

    void push() {
        auto& s = hitfx::g_settings;
        s.which = static_cast<hitfx::mode>(mode_.index());
        s.glow  = glow_.value();
        s.rainbow = rainbow_.value();
        const auto c = color_.value();
        s.col_r = c.r; s.col_g = c.g; s.col_b = c.b; s.col_a = c.a;
        s.scale = scale_.value();
        s.range = range_.value();
    }

    void onEnable() override {
        push();
        hitfx::g_enabled = true;
    }
    void onDisable() override {
        hitfx::g_enabled = false;
    }

    void drawImGui() override {
        Module::drawImGui();
        push();
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::HitFx)
