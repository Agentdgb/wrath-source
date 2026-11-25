// watermark.cpp — Wrath branding and session information.
//
// Two words on one line, sitting on the same dual-Kawase frosted-glass
// pipeline the arraylist uses. Split like the arraylist too — glass shader
// runs BEFORE NewFrame with raw GL, text runs INSIDE the ImGui frame on
// the foreground draw list.

#include "watermark.h"
#include "../arraylist/al_glass.h"
#include "../arraylist/font_pool.h"
#include "../arraylist/arraylist.h"    // for g_settings.blur_passes
#include "core/profile/PlayerProfile.h"
#include "vgui/bridge.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace dt::modules::watermark {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};
writers  g_writers{};

namespace {

// Dedicated glass instance — arraylist keeps its own so both can capture /
// blur independently without stomping each other's FBOs.
al::al_glass g_glass{};

struct cached_layout {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    bool  valid = false;
};
cached_layout g_layout{};

float g_override_x = std::numeric_limits<float>::quiet_NaN();
float g_override_y = std::numeric_limits<float>::quiet_NaN();

float elapsed_seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

std::string format_session_time(float seconds)
{
    const int total = std::max(0, static_cast<int>(seconds));
    const int h = std::min(99, total / 3600);
    const int m = (total / 60) % 60;
    const int s = total % 60;
    char out[32];
    std::snprintf(out, sizeof(out), "%02dh · %02dm · %02ds", h, m, s);
    return out;
}

// ── HSL derive (mirrors the arraylist so the gradient stays harmonious).
void rgb_to_hsl(float r, float g, float b, float& h, float& s, float& l)
{
    const float mx = std::max({ r, g, b });
    const float mn = std::min({ r, g, b });
    const float d  = mx - mn;
    l = (mx + mn) * 0.5f;
    if (d < 1e-6f) { h = 0.f; s = 0.f; return; }
    s = (l < 0.5f) ? d / (mx + mn) : d / (2.f - mx - mn);
    if      (mx == r) h = ((g - b) / d) + (g < b ? 6.f : 0.f);
    else if (mx == g) h = ((b - r) / d) + 2.f;
    else              h = ((r - g) / d) + 4.f;
    h /= 6.f;
}
static float hue_to_channel(float p, float q, float t)
{
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    if (t < 1.f/6.f) return p + (q - p) * 6.f * t;
    if (t < 1.f/2.f) return q;
    if (t < 2.f/3.f) return p + (q - p) * (2.f/3.f - t) * 6.f;
    return p;
}
void hsl_to_rgb(float h, float s, float l, float& r, float& g, float& b)
{
    if (s < 1e-6f) { r = g = b = l; return; }
    const float q = (l < 0.5f) ? l * (1.f + s) : l + s - l * s;
    const float p = 2.f * l - q;
    r = hue_to_channel(p, q, h + 1.f/3.f);
    g = hue_to_channel(p, q, h);
    b = hue_to_channel(p, q, h - 1.f/3.f);
}
/* Derive the second gradient stop from the user's accent so the two-stop
   sweep always reads as "one colour breathing" rather than a random pair.

   Earlier we pushed 35° of hue, which routinely produced ugly pairs — blue
   → cyan-green, red → yellow-brown. Two changes make the result cleaner:

     * SHIFT THE HUE LESS (≈20°). Analogous, not jumping to a neighbouring
       primary. Same colour family, closer to a shimmer than a rainbow.

     * SHIFT LIGHTNESS MORE (±0.22). With a smaller hue delta the eye
       needs a brightness delta to see the animation; this keeps the two
       stops visibly distinct without needing to leave the hue family.

     * COLLAPSE HUE-SHIFT WHEN THE INPUT IS NEAR-GREY. If s < 0.15 we're
       basically monochromatic; only shift lightness (a hue shift on
       a near-grey looks like a random tint appearing).
*/
void derive_second_stop(float r, float g, float b, float& r2, float& g2, float& b2)
{
    float h, s, l;
    rgb_to_hsl(r, g, b, h, s, l);

    if (s > 0.15f) {
        h += 0.056f;   // ~20 degrees on the wheel
        if (h > 1.f) h -= 1.f;
        // Slight saturation nudge so a very desaturated input still shows
        // some colour in its shimmer partner.
        s = std::clamp(s + 0.05f, 0.f, 1.f);
    }

    // Larger lightness delta compensates for the smaller hue delta.
    l = (l < 0.5f) ? std::min(1.f, l + 0.22f) : std::max(0.f, l - 0.22f);

    hsl_to_rgb(h, s, l, r2, g2, b2);
}

ImU32 pack_rgba(float r, float g, float b, float a)
{
    auto b8 = [](float v) {
        v = std::clamp(v, 0.f, 1.f);
        return static_cast<int>(v * 255.f + 0.5f);
    };
    return IM_COL32(b8(r), b8(g), b8(b), b8(a));
}

/* Per-glyph gradient — evaluated at each glyph's screen centre using the
   same warp / ping-pong formula the arraylist uses, so both surfaces
   animate in lockstep and feel like one system. */
ImU32 gradient_at(float frag_x, float frag_y, float time,
    float r1, float g1, float b1, float r2, float g2, float b2)
{
    (void)frag_y;
    constexpr float speed = 0.28f;   // matches arraylist speed
    // Local horizontal wavelength: the accent visibly travels across each
    // username instead of changing imperceptibly over the whole screen.
    const float coord = frag_x / 72.f;
    const float warp  = 0.10f * std::sin(coord * 6.2831853f);
    float f = std::fmod(coord + warp - time * speed, 1.f);
    if (f < 0.f) f += 1.f;
    const float scaled = f * 2.f;
    const int   seg = static_cast<int>(scaled);
    const float b = scaled - static_cast<float>(seg);
    const float sb = 0.5f - 0.5f * std::cos(b * 3.14159265f);

    const float lo_r = seg < 1 ? r1 : r2;
    const float lo_g = seg < 1 ? g1 : g2;
    const float lo_b = seg < 1 ? b1 : b2;
    const float hi_r = seg < 1 ? r2 : r1;
    const float hi_g = seg < 1 ? g2 : g1;
    const float hi_b = seg < 1 ? b2 : b1;

    return pack_rgba(lo_r + (hi_r - lo_r) * sb,
                     lo_g + (hi_g - lo_g) * sb,
                     lo_b + (hi_b - lo_b) * sb,
                     1.f);
}

} // namespace

/* ── Render pipeline ─────────────────────────────────────────────────── */

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    g_layout.valid = false;

    if (!g_enabled || display_w < 2 || display_h < 2) return;
    if (g_settings_provider) g_settings_provider(g_settings);

    auto& pool = arraylist::shared_pool();
    if (!pool.ensure()) return;

    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    // Compact by design — 15px base × dpi × user scale keeps it "small" like
    // the Drip-Lite reference the user showed.
    const float title_size = 15.f * dpi * g_settings.scale;
    const float ver_size   = 12.f * dpi * g_settings.scale;

    ImFont* title_f = pool.get(title_size);
    ImFont* ver_f   = pool.get(ver_size);
    if (!title_f || !ver_f) return;
    const float logo_w = title_size * 1.80f + 5.f * dpi * g_settings.scale +
                         title_f->CalcTextSizeA(title_size, FLT_MAX, -1.f, "Nexus").x;

    const std::string minecraft_user = dt::profile::name();
    const char* user = minecraft_user.empty() ? "Player" : minecraft_user.c_str();

    const std::string session = format_session_time(elapsed_seconds());
    const ImVec2 us  = ver_f  ->CalcTextSizeA(ver_size,   FLT_MAX, -1.f, user);
    const ImVec2 ts  = ver_f  ->CalcTextSizeA(ver_size,   FLT_MAX, -1.f, "99h · 99m · 99s");

    const float pad_x    = 9.f * dpi * g_settings.scale;
    const float pad_y    = 5.f * dpi * g_settings.scale;
    const float gap      = 5.f * dpi * g_settings.scale;
    const float sep_gap  = 8.f * dpi * g_settings.scale;   // padding on each side of the divider

    // Logo platform + username/time platform, with a deliberate breathing gap.
    const float body_w = logo_w + 2.f * sep_gap + us.x + gap + ts.x;
    const float body_h = std::max({ title_size, ver_size, us.y });

    const float w = body_w + pad_x * 2.f;
    const float h = body_h + pad_y * 2.f;

    // Default anchor: top-left with a small margin.
    float x, y;
    if (!std::isnan(g_override_x) && !std::isnan(g_override_y)) {
        x = g_override_x; y = g_override_y;
    } else {
        x = 12.f;
        y = 12.f;
    }

    g_layout.x = x; g_layout.y = y; g_layout.w = w; g_layout.h = h;
    g_layout.valid = true;

    // ── Glass pill: same knobs as the arraylist ──
    if (g_settings.acrylic && g_glass.available()) {
        const float y_bottom = static_cast<float>(display_h) - (y + h);
        std::vector<al::al_rect> rects;
        /* Radius follows the arraylist's exact formula so both surfaces
           agree on what "rounding = 2" looks like: 3 px base × scale ×
           rounding, capped just under half the smaller side. At the max
           setting the corners round to ~6 px on 1080p — corners, not a
           full pill. */
        const float radius_base = 3.f * g_settings.scale * g_settings.rounding;
        const float cap = std::max(0.f, std::min(w, h) * 0.5f - 1.f);
        const float rad = std::min(radius_base, cap);
        const float platform_gap = sep_gap;
        const float logo_platform_w = pad_x + logo_w + sep_gap;
        const float content_x = x + logo_platform_w + platform_gap;
        const float content_w = std::max(1.f, x + w - pad_x - content_x);
        rects.emplace_back(x, y_bottom, logo_platform_w, h, rad, rad, rad, rad);
        rects.emplace_back(content_x, y_bottom, content_w, h, rad, rad, rad, rad);

        const int pad = static_cast<int>(std::ceil(g_settings.scale * 14.0));
        const int bx  = static_cast<int>(std::floor(x)) - pad;
        const int by  = static_cast<int>(std::floor(y_bottom)) - pad;
        const int bw  = static_cast<int>(std::ceil(w)) + 2 * pad;
        const int bh  = static_cast<int>(std::ceil(h)) + 2 * pad;

        const al::al_tint tint{ 0.f, 0.f, 0.f, 95.f / 255.f };
        const float aa_pixels   = std::max(1.5f, 1.75f * g_settings.scale);
        const int   blur_passes = arraylist::g_settings.blur_passes;
        g_glass.render(rects,
            display_w, display_h,
            bound_fb,
            bx, by, bw, bh,
            blur_passes,
            /*brightness=*/0.65f,
            tint,
            /*edge_softness=*/aa_pixels,
            /*shadow_radius=*/9.f * g_settings.scale,
            /*shadow_strength=*/0.20f,
            /*shadow_gamma=*/1.35f);
    }
}

void render_text(int display_w, int display_h)
{
    if (!g_enabled || !g_layout.valid) return;

    auto& pool = arraylist::shared_pool();
    if (!pool.ensure()) return;

    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float title_size = 15.f * dpi * g_settings.scale;
    const float ver_size   = 12.f * dpi * g_settings.scale;
    const float pad_x   = 9.f * dpi * g_settings.scale;
    const float pad_y   = 5.f * dpi * g_settings.scale;
    const float gap     = 5.f * dpi * g_settings.scale;
    const float sep_gap = 8.f * dpi * g_settings.scale;

    ImFont* title_f = pool.get(title_size);
    ImFont* ver_f   = pool.get(ver_size);
    if (!title_f || !ver_f) return;
    const float logo_w = title_size * 1.80f + 5.f * dpi * g_settings.scale +
                         title_f->CalcTextSizeA(title_size, FLT_MAX, -1.f, "Nexus").x;
    (void)pad_y;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    dl->PushClipRect(ImVec2(0.f, 0.f),
        ImVec2(static_cast<float>(display_w), static_cast<float>(display_h)), false);

    const float x = g_layout.x;
    const float y = g_layout.y;
    const float w = g_layout.w;
    const float h = g_layout.h;

    if (!g_settings.acrylic || !g_glass.available()) {
        const float radius_base = 3.f * g_settings.scale * g_settings.rounding;
        const float cap = std::max(0.f, std::min(w, h) * 0.5f - 1.f);
        const float platform_gap = sep_gap;
        const float logo_platform_w = pad_x + logo_w + sep_gap;
        const float content_x = x + logo_platform_w + platform_gap;
        const float content_w = std::max(1.f, x + w - pad_x - content_x);
        const float rad = std::min(radius_base, cap);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + logo_platform_w, y + h),
            IM_COL32(12, 12, 12, 255), rad);
        dl->AddRectFilled(ImVec2(content_x, y), ImVec2(content_x + content_w, y + h),
            IM_COL32(12, 12, 12, 255), rad);
    }

    const float t = elapsed_seconds();

    // Derive the gradient's second stop from the user's accent so the
    // two-stop shimmer stays harmonious with whatever colour is picked.
    float r1 = g_settings.accent_r, g1 = g_settings.accent_g, b1 = g_settings.accent_b;
    float r2, g2, b2;
    derive_second_stop(r1, g1, b1, r2, g2, b2);

    /* Single neutral hairline outline. An earlier version stacked an
       accent-tinted halo underneath and it leaked colour past the
       edges of the pill — the shader's own edge-softness stops just
       inside the SDF and the coloured halo sat visibly outside it. A
       plain white low-alpha stroke matches the shader's own soft edge
       and reads as one clean silhouette. */
    {
        const float radius_base = 3.f * g_settings.scale * g_settings.rounding;
        const float cap = std::max(0.f, std::min(w, h) * 0.5f - 1.f);
        const float rad = std::min(radius_base, cap);
        const float platform_gap = sep_gap;
        const float logo_platform_w = pad_x + logo_w + sep_gap;
        const float content_x = x + logo_platform_w + platform_gap;
        const float content_w = std::max(1.f, x + w - pad_x - content_x);
        const ImU32 border = IM_COL32(255, 255, 255, 12);
        dl->AddRect(ImVec2(x, y), ImVec2(x + logo_platform_w, y + h), border, rad, 0, 1.f);
        dl->AddRect(ImVec2(content_x, y), ImVec2(content_x + content_w, y + h), border, rad, 0, 1.f);
    }

    dl->PushTextureID(pool.texture_id());

    const float logo_x = x + pad_x;
    const float logo_y = y + h * 0.5f;
    const int logo_px = std::max(8, static_cast<int>(std::lround(title_size * 1.8f)));

    const char* word = "Nexus";
    const ImVec2 ws = title_f->CalcTextSizeA(title_size, FLT_MAX, -1.f, word);
    const float word_x = logo_x + title_size * 1.80f + gap;
    const float word_y = y + (h - ws.y) * .5f;

    if (g_settings.gradient_text) {
        /* Per-glyph gradient — each letter samples the same warp-and-
           ping-pong formula the arraylist uses at its own screen centre.
           Reads as a smooth fluid gradient sweeping through the word. */
        float cursor_x = word_x;
        const char* s = word;
        while (*s) {
            const char one[2] = { *s, 0 };
            const ImVec2 sz = title_f->CalcTextSizeA(title_size, FLT_MAX, -1.f, one);
            const float glyph_cx = cursor_x + sz.x * 0.5f;
            const float glyph_cy = word_y + ws.y * 0.5f;
            const float frag_y = static_cast<float>(display_h) - glyph_cy;
            const ImU32 col = gradient_at(glyph_cx, frag_y, t, r1, g1, b1, r2, g2, b2);
            dl->AddText(title_f, title_size, ImVec2(cursor_x, word_y), col, one);
            cursor_x += sz.x;
            ++s;
        }
    } else {
        // Flat accent colour — one draw, no per-glyph sampling.
        const ImU32 col = pack_rgba(r1, g1, b1, g_settings.accent_a);
        dl->AddText(title_f, title_size, ImVec2(word_x, word_y), col, word);
    }

    // Version tag — muted white, tight next to the word.
    const float ver_x = logo_x + logo_w;

    // Username — same muted tone as the version. Drawn INSIDE the font
    // texture push scope so the atlas is still active.
    const std::string minecraft_user = dt::profile::name();
    const char* user = minecraft_user.empty() ? "Player" : minecraft_user.c_str();
    const ImVec2 us = ver_f->CalcTextSizeA(ver_size, FLT_MAX, -1.f, user);
    const float sep_x  = ver_x + sep_gap;
    const float user_x = sep_x + sep_gap;
    const float user_y = y + (h - us.y) * 0.5f + 1.f;
    float user_cursor = user_x;
    const float wave_time = t * 0.75f;
    while (*user) {
        const char glyph[2] = { *user, 0 };
        const ImVec2 glyph_size = ver_f->CalcTextSizeA(ver_size, FLT_MAX, -1.f, glyph);
        const ImU32 glyph_col = gradient_at(user_cursor, user_y, wave_time,
            r1, g1, b1, r2, g2, b2);
        dl->AddText(ver_f, ver_size, ImVec2(user_cursor, user_y), glyph_col, glyph);
        user_cursor += glyph_size.x;
        ++user;
    }

    const std::string session_text = format_session_time(t);
    const ImVec2 session_size = ver_f->CalcTextSizeA(ver_size, FLT_MAX, -1.f, session_text.c_str());
    const float session_dot_x = user_cursor + gap * 0.45f;
    const float session_x = user_cursor + gap;
    dl->AddText(ver_f, ver_size, ImVec2(session_x, y + (h - session_size.y) * 0.5f + 1.f),
        IM_COL32(205, 208, 216, 220), session_text.c_str());

    dl->PopTextureID();

    // Use the same vector logo texture as the GUI, tinted by the selected accent.
    const unsigned long long logo_tex = dt::vgui::bridge::iconTexture("logo", logo_px);
    if (logo_tex) {
        const float logo_size = static_cast<float>(logo_px);
        dl->PushTextureID(static_cast<ImTextureID>(logo_tex));
        // Four low-alpha offset passes make a restrained accent glow. The
        // source logo remains crisp on top and keeps the GUI's exact shape.
        const ImVec2 glow_offsets[] = {
            ImVec2(-1.5f, 0.f), ImVec2(1.5f, 0.f),
            ImVec2(0.f, -1.5f), ImVec2(0.f, 1.5f)
        };
        const ImU32 glow = pack_rgba(r1, g1, b1, g_settings.accent_a * 0.16f);
        for (const ImVec2& offset : glow_offsets) {
            dl->AddImage(static_cast<ImTextureID>(logo_tex),
                ImVec2(logo_x + offset.x, logo_y - logo_size * 0.5f + offset.y),
                ImVec2(logo_x + logo_size + offset.x, logo_y + logo_size * 0.5f + offset.y),
                ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), glow);
        }
        dl->AddImage(static_cast<ImTextureID>(logo_tex),
            ImVec2(logo_x, logo_y - logo_size * 0.5f),
            ImVec2(logo_x + logo_size, logo_y + logo_size * 0.5f),
            ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
            pack_rgba(1.f, 1.f, 1.f, g_settings.accent_a));
        dl->PopTextureID();
    }

    dl->AddCircleFilled(ImVec2(session_dot_x, y + h * 0.5f),
        std::max(1.f, dpi * g_settings.scale * 0.9f),
        IM_COL32(145, 148, 156, 145), 8);

    /* Separator drawn OUTSIDE the font-atlas texture push. AddLine samples
       the "white pixel" UV from DrawListSharedData — that UV points into
       the DEFAULT ImGui atlas, not the pool atlas. Under the pool push
       the shader sampled that UV out of an unrelated position in the
       pool atlas → invisible / wrong colour, which is exactly why the
       separator wasn't showing up. Popping first restores the default
       texture so the line renders as an actual solid stroke. */
    dl->PopClipRect();
}

void query_bbox(float& x, float& y, float& w, float& h)
{
    if (!g_layout.valid) { x = y = w = h = 0.f; return; }
    x = g_layout.x; y = g_layout.y; w = g_layout.w; h = g_layout.h;
}

void set_top_left(float x, float y)
{
    g_override_x = x;
    g_override_y = y;
}

} // namespace dt::modules::watermark

