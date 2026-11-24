// TargetHudModule.cpp — the Visuals module wrapper for the Target HUD.

#include "hud_font.h"
#include "target_source.h"
#include "targethud.h"

#include "core/module/Module.h"

#include <initializer_list>
#include <vector>

namespace vanta::modules {

namespace {

/* @the option labels come from the font pool, never retyped here.

   EnumSetting's other constructor takes an initializer_list, which has to be a
   braced literal — it cannot be built from a pointer and a count, and a helper
   that returns one hands back a dangling array the moment it returns. The
   pointer/count overload exists for exactly this: a setting whose options are
   owned somewhere else. Retyping the eleven names instead would compile forever
   and drift silently, at which point the dropdown says "Tahoma" and the HUD
   draws Consolas. */

} // namespace

class TargetHud : public Module {
public:
    TargetHud()
        : Module("Target Display",
                 "Who you are fighting",
                 Category::Visuals)
    {
        registerSetting(&font_);
        registerSetting(&show_);
        registerSetting(&scale_);
        registerSetting(&rounding_);
        registerSetting(&linger_);
        registerSetting(&range_);
        registerSetting(&distance_);
        registerSetting(&follow_);

        /* Read live off the settings on every frame, the same way the watermark
           does. The provider runs on the game thread inside the render pass and
           the settings are edited on that same thread through the bridge, at a
           different point in the frame — so there is nothing to synchronise and
           nothing to keep a second copy of. */
        targethud::g_settings_provider = [this](targethud::settings& out) {
            out.font = font_.index();
            out.show = show_.index();
            out.scale = scale_.value();
            out.rounding = rounding_.value();
            out.linger = linger_.value();
            out.range = range_.value();
            out.show_distance = distance_.value();
            out.follow = follow_.value();
        };
    }

    ~TargetHud() override
    {
        targethud::g_settings_provider = {};
        targethud::g_enabled = false;
    }

private:
    EnumSetting  font_          { this, "Font",
                                  targethud::face_names(), targethud::face_count, 0 };
    EnumSetting  show_          { this, "Show", { "On Target", "Always" }, 0 };
    FloatSetting scale_         { this, "Scale",    1.2f, 0.5f, 2.5f, 0.05f };
    FloatSetting rounding_      { this, "Rounding", 1.0f, 0.0f, 2.0f, 0.1f  };
    FloatSetting linger_        { this, "Linger",   4.0f, 0.5f, 15.0f, 0.5f };
    FloatSetting range_         { this, "Range",   16.0f, 4.0f, 64.0f, 1.0f  };
    BoolSetting  distance_      { this, "Distance", true };
    BoolSetting  follow_        { this, "Follow Target", false };

    void onEnable() override
    {
        targethud::g_enabled = true;
    }

    void onDisable() override
    {
        targethud::g_enabled = false;
        targethud::reset();
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::TargetHud)
