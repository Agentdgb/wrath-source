// OutlineModule.cpp — the Visuals module wrapper for the Outline.

#include "outline.h"

#include "core/module/Module.h"

namespace dt::modules {

class Outline : public Module {
public:
    Outline()
        : Module("Outline",
                 "Draws a line around nearby players",
                 Category::Visuals)
    {
        registerSetting(&thickness_);
        registerSetting(&colour_);
        registerSetting(&glow_);
        registerSetting(&glow_size_);
        registerSetting(&glow_strength_);

        /* Read live off the settings every frame, the same way the Radar and the
           Target HUD do. The provider runs on the render thread inside the draw
           pass and the settings are edited on that same thread through the
           bridge, at a different point in the frame — so there is nothing to
           synchronise and nothing to keep a second copy of. */
        outline::g_settings_provider = [this](outline::settings& out) {
            out.thickness = thickness_.value();
            out.glow      = glow_.value();
            out.glow_size = glow_size_.value();
            out.glow_alpha = glow_strength_.value();

            const RGBA c = colour_.value();
            out.col_r = c.r;
            out.col_g = c.g;
            out.col_b = c.b;
            out.col_a = c.a;
        };
    }

    ~Outline() override
    {
        outline::g_settings_provider = {};
        outline::g_enabled = false;
    }

    void onInit() override { outline::install(); }

private:
    FloatSetting thickness_ { this, "Thickness",  0.50f, 0.20f,  2.00f, 0.05f };
    ColorSetting colour_    { this, "Enemy color",
                              RGBA{ 1.f, 49.f / 255.f, 61.f / 255.f, 1.f } };
    BoolSetting  glow_      { this, "Glow",       true };
    FloatSetting glow_size_ { this, "Glow size",  2.50f, 1.00f, 20.00f, 0.10f };

    /* The intensity control the panel never had — the engine field behind it
       was declared and uploaded but never once assigned, so the halo's
       brightness was a constant nobody could reach.

       Topped at 1.00 rather than higher on purpose. The halo is screened onto
       the world, and that blend is only provably bounded while its source stays
       inside [0,1]; across the whole settings space the profile peaks at 0.927
       of this value, so 1.00 leaves 7% of headroom and anything above about
       1.08 would clip the red channel flat over a band. */
    FloatSetting glow_strength_ { this, "Glow strength", 0.85f, 0.05f, 1.00f, 0.05f };

    void onEnable() override  { outline::g_enabled = true;  }
    void onDisable() override { outline::g_enabled = false; }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::Outline)
