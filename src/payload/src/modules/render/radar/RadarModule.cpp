// RadarModule.cpp — the Visuals module wrapper for the Radar.

#include "radar.h"
#include "radar_source.h"

#include "core/module/Module.h"

namespace dt::modules {

class Radar : public Module {
public:
    Radar()
        : Module("Radar",
                 "Top-down dial of who is near",
                 Category::Visuals)
    {
        registerSetting(&mode_);
        registerSetting(&range_);
        registerSetting(&scale_);
        registerSetting(&rounding_);
        registerSetting(&colour_);
        registerSetting(&height_);

        /* Read live off the settings on every frame, the same way the watermark
           and the Target HUD do. The provider runs on the game thread inside the
           render pass and the settings are edited on that same thread through the
           bridge, at a different point in the frame — so there is nothing to
           synchronise and nothing to keep a second copy of. */
        radar::g_settings_provider = [this](radar::settings& out) {
            out.mode = mode_.index();
            out.range = range_.value();
            out.scale = scale_.value();
            out.rounding = rounding_.value();
            out.show_height = height_.value();

            const RGBA blip = colour_.value();
            out.blip_r = blip.r;
            out.blip_g = blip.g;
            out.blip_b = blip.b;
            out.blip_a = blip.a;

        };
    }

    ~Radar() override
    {
        radar::g_settings_provider = {};
        radar::g_enabled = false;
    }

private:
    EnumSetting  mode_       { this, "Mode", { "Arrow", "Dot" }, 0 };

    /* Breeze's own default and limits. */
    FloatSetting range_      { this, "Range",    60.f, 10.f, 100.f, 1.f  };

    FloatSetting scale_      { this, "Scale",    1.0f, 0.5f, 2.5f,  0.05f };
    FloatSetting rounding_   { this, "Rounding", 1.0f, 0.0f, 2.0f,  0.1f  };

    /* Breeze's pointer colour, kept rather than swapped for the client accent:
       the radar is read against the world, not against the rest of the client. */
    ColorSetting colour_     { this, "Color",
                               RGBA{ 230.f / 255.f, 34.f / 255.f, 100.f / 255.f, 1.f } };

    BoolSetting  height_     { this, "Height Tint", true };

    void onEnable() override
    {
        radar::g_enabled = true;
    }

    void onDisable() override
    {
        radar::g_enabled = false;
        radar::reset();
    }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::Radar)
