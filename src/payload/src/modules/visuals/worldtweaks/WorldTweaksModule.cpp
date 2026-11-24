// WorldTweaksModule.cpp — ClickGui glue for the WorldTweaks engine.
//
// Same shape as BlockEspModule: the engine (worldtweaks.cpp) owns all JNI and
// state; this class owns the settings and mirrors them into the engine's
// settings POD each tick, then drives the engine from GameLoopEvent.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JvmContext.h"

#include "modules/visuals/worldtweaks/worldtweaks.h"

namespace vanta::modules {

class WorldTweaks : public Module {
public:
    WorldTweaks() : Module("WorldTweaks",
                           "Client-side time-of-day override plus ambient particles. "
                           "Visual only — the server's real time is untouched.",
                           Category::Visuals) {
        registerSetting(&timeEnabled_);
        registerSetting(&timeTicks_);
        registerSetting(&timeSpeed_);
        registerSetting(&ambient_);
        registerSetting(&color_);
        registerSetting(&rainbow_);
        registerSetting(&size_);
        registerSetting(&ambientDensity_);
        registerSetting(&ambientRadius_);
    }

    void onInit() override {
        worldtweaks::install();
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){
            if (!isEnabled()) return;
            mirror();
            ::vanta::jvm::JniScope s;
            if (s) worldtweaks::tick(s.get(), e.deltaMs);
        });
        // Drop the server's WorldTimeUpdate while the time override is on, so it
        // can't overwrite our time for a frame (the sky-flash). Runs on the netty
        // thread; the engine gates on enabled + time_enabled itself.
        listen<events::PacketReceiveEvent>([](events::PacketReceiveEvent& e){
            if (worldtweaks::wants_cancel_time_packet(e.env, e.packet))
                e.cancel();
        });
    }

    void onEnable() override {
        mirror();
        worldtweaks::g_enabled.store(true, std::memory_order_relaxed);
    }
    void onDisable() override {
        worldtweaks::g_enabled.store(false, std::memory_order_relaxed);
        worldtweaks::on_disabled();
    }

private:
    void mirror() {
        auto& g = worldtweaks::g_settings;
        g.time_enabled    = timeEnabled_.value();
        g.time_ticks      = timeTicks_.value();
        g.time_speed      = timeSpeed_.value();
        g.ambient_mode    = ambient_.index();
        g.ambient_density = ambientDensity_.value();
        g.ambient_radius  = ambientRadius_.value();
        const auto c      = color_.value();
        g.col_r = c.r; g.col_g = c.g; g.col_b = c.b;
        g.rainbow         = rainbow_.value();
        g.size            = size_.value();
    }

    // Time: a master toggle, a 0..24000 time-of-day slider, and a speed. Speed 0
    // freezes at the slider; >0 makes the day advance at that many ticks/second.
    BoolSetting  timeEnabled_   {this, "Time", false};
    IntSetting   timeTicks_     {this, "TimeOfDay", 6000, 0, 24000};
    FloatSetting timeSpeed_     {this, "TimeSpeed", 0.f, 0.f, 300.f, 1.f};

    // Particle option order MUST match AmbientMode in worldtweaks.h. These are
    // our own particles drawn on the overlay (world-anchored glow), not MC's —
    // so they show with MC particles off, and Color/Rainbow/Size style them.
    EnumSetting  ambient_       {this, "Particles",
                                 {"Off","Fireflies","Embers","Snow","Sparkles","Magic","Souls"}, 0};
    ColorSetting color_         {this, "Color", RGBA{1.0f, 0.82f, 0.30f, 1.0f}};
    BoolSetting  rainbow_       {this, "Rainbow", false};
    FloatSetting size_          {this, "Size", 1.0f, 0.3f, 3.0f, 0.1f};
    IntSetting   ambientDensity_{this, "Density", 14, 1, 60};
    IntSetting   ambientRadius_ {this, "Radius", 12, 3, 32};
    // No Debug switch in the panel, by request; worldtweaks::settings::debug
    // stays at its default (off). Hook and resolution status still go to
    // vanta.log under [worldtweaks].
};

VANTA_REGISTER_MODULE(WorldTweaks);

} // namespace vanta::modules
