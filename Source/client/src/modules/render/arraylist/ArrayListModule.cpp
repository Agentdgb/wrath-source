// ArrayListModule.cpp — Vanta module wrapper for the ArrayList HUD.
//
// Owns the settings surface. Installs a settings_provider callback that the
// renderer pulls once per frame, so slider changes from the ClickGui take
// effect immediately (unlike a "push on onEnable" cache).

#include "arraylist.h"

#include "core/module/Module.h"

namespace vanta::modules {

class ArrayList : public Module {
public:
    ArrayList()
        : Module("ArrayList",
                 "Your enabled modules, on screen",
                 Category::Visuals)
    {
        registerSetting(&style_);
        registerSetting(&background_);
        registerSetting(&glow_);
        registerSetting(&lowercase_);
        registerSetting(&show_settings_);
        registerSetting(&scale_);
        registerSetting(&rounding_);
        registerSetting(&blur_passes_);
        registerSetting(&gradient_);
        registerSetting(&colour_);
        registerSetting(&cat_combat_);
        registerSetting(&cat_movement_);
        registerSetting(&cat_visuals_);
        registerSetting(&cat_misc_);

        /* The renderer runs on the GL thread and never gets our onEnable —
           on-slider-change events (bridge::applyBool/applyNumeric write the
           setting directly). Register a pull callback so the render frame
           always sees the latest values, no matter which path edited them. */
        /* Writers — the HUD editor calls these when the user drags a slider
           or clicks a toggle in the sub-view, and the values land in the
           real vanta::Setting objects so they persist via the normal
           config save/load. */
        arraylist::g_setting_writers.bool_ = [this](const char* name, bool v) {
            const std::string_view n{ name };
            if      (n == "Background")            background_.set(v);
            else if (n == "Glow")                  glow_.set(v);
            else if (n == "Lowercase")             lowercase_.set(v);
            else if (n == "Show Module Settings")  show_settings_.set(v);
            else if (n == "Gradient")              gradient_.set(v);
            else if (n == "Combat")                cat_combat_.set(v);
            else if (n == "Movement")              cat_movement_.set(v);
            else if (n == "Visuals")               cat_visuals_.set(v);
            else if (n == "Misc")                  cat_misc_.set(v);
        };
        arraylist::g_setting_writers.float_ = [this](const char* name, float v) {
            const std::string_view n{ name };
            if      (n == "Scale")     scale_.set(v);
            else if (n == "Rounding")  rounding_.set(v);
        };
        arraylist::g_setting_writers.int_ = [this](const char* name, int v) {
            const std::string_view n{ name };
            if (n == "Blur") blur_passes_.set(v);
            else if (n == "Style") style_.set(v);
        };
        arraylist::g_setting_writers.color = [this](const char* name, float r, float g, float b, float a) {
            const std::string_view n{ name };
            if (n == "Colour") colour_.set(RGBA{ r, g, b, a });
        };

        arraylist::g_settings_provider = [this](arraylist::settings& out) {
            out.style          = style_.index();
            out.background     = background_.value();
            out.glow           = glow_.value();
            out.lowercase      = lowercase_.value();
            out.show_settings  = show_settings_.value();
            out.scale          = scale_.value();
            out.rounding       = rounding_.value();
            out.blur_passes    = blur_passes_.value();

            out.gradient_text  = gradient_.value();
            const auto c = colour_.value();
            out.text_r = c.r; out.text_g = c.g; out.text_b = c.b; out.text_a = c.a;

            out.cat_combat     = cat_combat_.value();
            out.cat_movement   = cat_movement_.value();
            out.cat_visuals    = cat_visuals_.value();
            out.cat_misc       = cat_misc_.value();
        };
    }

    ~ArrayList() override {
        arraylist::g_settings_provider = {};
    }

private:
    EnumSetting  style_         { this, "Style", { "Default", "Mono" }, 0 };
    BoolSetting  background_    { this, "Background",        true  };
    BoolSetting  glow_          { this, "Glow",              true  };
    BoolSetting  lowercase_     { this, "Lowercase",         false };
    BoolSetting  show_settings_ { this, "Show Module Settings", true };
    FloatSetting scale_         { this, "Scale",             1.0f, 0.5f, 3.0f, 0.05f };
    FloatSetting rounding_      { this, "Rounding",          1.0f, 0.0f, 2.0f, 0.1f  };
    IntSetting   blur_passes_   { this, "Blur",              2,    1,    5           };

    /* One accent colour and one Gradient toggle — when Gradient is on the
       renderer derives the second stop from this same accent by shifting
       hue ~35° and nudging saturation/lightness, so any pick pairs with
       itself: green pairs with yellow-green, blue with cyan, red with
       orange, etc. */
    BoolSetting  gradient_      { this, "Gradient",          true };
    ColorSetting colour_        { this, "Colour",
                                  RGBA{ 70.f/255.f, 130.f/255.f, 1.f, 1.f } };

    BoolSetting  cat_combat_    { this, "Combat",            true  };
    BoolSetting  cat_movement_  { this, "Movement",          true  };
    BoolSetting  cat_visuals_   { this, "Visuals",           false };
    BoolSetting  cat_misc_      { this, "Misc",              true  };

    void onEnable() override {
        arraylist::g_enabled = true;
    }
    void onDisable() override {
        arraylist::g_enabled = false;
        /* Keep the shader + FBOs alive — see arraylist.cpp for the note on
           why teardown-on-disable caused the "black flash" the user was
           reporting. Real teardown runs from al_glass's destructor when
           the module is unregistered at shutdown. */
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::ArrayList)
