// ChamsModule.cpp — Dt module wrapper for the Chams engine.
//
// NO SETTINGS. The module is a toggle and nothing else.
//
// Everything that used to be exposed turned out to be either a workaround for a
// bug that is now fixed, or a way to break the feature:
//
//   Mode (Color/Normal)  — the colour mode swapped the skin for a flat fill.
//                          What people actually want here is the player as they
//                          really look, through walls; the flat variant was a
//                          second thing to keep working for no gain.
//   Wall Method          — "Depth Range" always won.
//   Skip Blended         — threw away the player's opaque body, because
//                          GL_BLEND is on essentially all frame.
//   Overlay Fallback     — an ImGui fill beside real players read as a glitch.
//   Range / Max Targets  — these bound the poll, and the poll is what marks
//                          players not-culled. Capping either made players
//                          vanish, which is the bug this module spent most of
//                          its bring-up chasing. Pinned to maximum, not
//                          offered.
//   Shading / Outer Layer / Split Visible / Hidden Color
//                        — all overlay-only, and the overlay is retired.

#include "chams.h"

#include "core/module/Module.h"

namespace dt::modules {

class Chams : public Module {
public:
    Chams()
        : Module("Chams", "See players through walls",
                 Category::Visuals)
    {
        push();
        chams::g_settings_provider = [this](chams::settings& s) { fill(s); };
    }
    ~Chams() override { chams::g_settings_provider = {}; }

    void onInit() override { chams::install(); }

private:
    static void fill(chams::settings& s)
    {
        // The poll must cover everyone: it is what keeps players marked
        // not-culled and their skins published.
        s.range       = 128.0f;
        s.max_targets = 256;
        s.showself    = false;
    }

    void push() { fill(chams::g_settings); }

    void onEnable() override  { push(); chams::g_enabled = true;  }
    void onDisable() override { chams::g_enabled = false; }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::Chams)
