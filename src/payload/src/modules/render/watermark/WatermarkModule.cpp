// WatermarkModule.cpp — Wrath module wrapper for the "Wrath" watermark.
//
// Compact by design: just Scale + Accent. Glass appearance mirrors the
// arraylist (blur_passes pulled live from arraylist::g_settings).

#include "watermark.h"

#include "core/module/Module.h"

namespace dt::modules {

class Watermark : public Module {
public:
    Watermark()
        : Module("Watermark",
                 "Overlay tag in the corner",
                 Category::Visuals)
    {
        registerSetting(&scale_);
        registerSetting(&rounding_);
        registerSetting(&gradient_);
        registerSetting(&accent_);
        registerSetting(&acrylic_);

        watermark::g_writers.bool_ = [this](const char* n, bool v) {
            const std::string_view s{n};
            if (s == "Gradient") gradient_.set(v);
        };
        watermark::g_writers.float_ = [this](const char* n, float v) {
            const std::string_view s{n};
            if      (s == "Scale")    scale_.set(v);
            else if (s == "Rounding") rounding_.set(v);
        };
        watermark::g_writers.color = [this](const char* n, float r, float g, float b, float a) {
            const std::string_view s{n};
            if (s == "Accent") accent_.set(RGBA{ r, g, b, a });
        };

        watermark::g_settings_provider = [this](watermark::settings& out) {
            out.scale         = scale_.value();
            out.rounding      = rounding_.value();
            out.gradient_text = gradient_.value();
            out.acrylic       = acrylic_.value();
            const auto c = accent_.value();
            out.accent_r = c.r; out.accent_g = c.g; out.accent_b = c.b; out.accent_a = c.a;
        };
    }
    ~Watermark() override {
        watermark::g_settings_provider = {};
        watermark::g_writers = {};
    }

private:
    FloatSetting scale_    { this, "Scale",    1.0f, 0.5f, 2.5f, 0.05f };
    FloatSetting rounding_ { this, "Rounding", 2.0f, 0.0f, 2.0f, 0.1f  };
    BoolSetting  gradient_ { this, "Gradient", true };
    BoolSetting  acrylic_  { this, "Blur", true };
    ColorSetting accent_   { this, "Accent",   RGBA{ 155.f/255.f, 75.f/255.f, 220.f/255.f, 1.f } };

    void onEnable()  override { watermark::g_enabled = true; }
    void onDisable() override { watermark::g_enabled = false; }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::Watermark)
