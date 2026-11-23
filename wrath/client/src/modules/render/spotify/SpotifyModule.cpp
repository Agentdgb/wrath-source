#include "spotify.h"
#include "spotify_worker.h"

#include "core/module/Module.h"

namespace vanta::modules {

class Spotify : public Module {
public:
    Spotify()
        : Module("Spotify",
                 "What you are listening to",
                 Category::Visuals)
    {
        registerSetting(&scale_);
        registerSetting(&background_);

        spotify::g_writers.float_ = [this](const char* n, float v) {
            const std::string_view s{n};
            if (s == "Scale") scale_.set(v);
        };

        // Appearance presets share artwork colors; other presentation defaults stay fixed.
        spotify::g_settings_provider = [this](spotify::settings& out) {
            out.background    = background_.index();
            out.scale         = scale_.value();
            out.rounding      = 2.0f;
            out.show_progress = true;
            out.blur          = true;
            out.accent_r = 1.f; out.accent_g = 1.f;
            out.accent_b = 1.f; out.accent_a = 1.f;
            out.spotify_client_id.clear();
            out.spotify_client_secret.clear();
        };
    }
    ~Spotify() override {
        spotify::g_settings_provider = {};
        spotify::g_writers = {};
    }

private:
    EnumSetting background_ {this, "Background", {"Artwork", "Ember", "Silk"}, 0};
    FloatSetting scale_ { this, "Scale", 1.0f, 0.5f, 2.5f, 0.05f };

    void onEnable()  override {
        spotify::g_enabled = true;
        spotify::worker::start();
    }
    void onDisable() override {
        spotify::g_enabled = false;
        spotify::worker::stop();
    }
};

}

VANTA_REGISTER_MODULE(vanta::modules::Spotify)
