// arraylist.cpp — layout + per-frame render for the ArrayList HUD.
//
// The layout is a faithful port of Vape/Breeze's ArrayListMod: rows sorted by
// width, right-anchored, with a stairstep left edge that "merges" adjacent
// widths through per-corner rounding. Backgrounds go through al_glass (real
// dual-Kawase glass); text goes through ImGui's foreground draw list with a
// CPU-computed animated gradient and a cheap angular-ring glow.

#include "arraylist.h"
#include "al_glass.h"
#include "font_pool.h"

#include "core/module/Module.h"
#include "core/module/ModuleManager.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/EnumSetting.h"
#include "core/setting/NumericSetting.h"
#include "core/setting/Setting.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dt::modules::arraylist {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};
setting_writers g_setting_writers{};

namespace {

/* Renderer state, all on the render thread. request_teardown() flips
   `pending_teardown` and the next render_frame invocation calls destroy(). */
al::al_glass         g_glass{};
bool                 g_pending_teardown = false;

/* Metadata always renders as a soft grey — Vape's default. Everything else
   about text colour comes from g_settings at frame time. */
constexpr ImU32 col_secondary = IM_COL32(180, 180, 190, 255);

inline ImU32 pack_rgba(float r, float g, float b, float a)
{
    auto to_byte = [](float v) {
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        return static_cast<int>(v * 255.f + 0.5f);
    };
    return IM_COL32(to_byte(r), to_byte(g), to_byte(b), to_byte(a));
}

struct row {
    std::string name;
    std::string meta;
    float name_width = 0.f;
    float meta_width = 0.f;
    float width      = 0.f;   // name + gap + meta (or just name)
};

/* The dedicated arraylist font pool is populated lazily on first render, and
   picked over the shared dt atlas because it holds bigger bakes at higher
   oversampling. */
ImFont* pick_font_for(float desired_canvas_size)
{
    return shared_pool().get(desired_canvas_size);
}

// Seconds since the module first rendered. Anchoring on module-start rather
// than std::chrono::steady_clock::now() would work too, but this way t=0 on
// first frame and we sidestep any floating-point precision drift for
// long-running sessions.
float elapsed_seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
}

std::string maybe_lower(std::string_view sv, bool lowercase)
{
    std::string out(sv);
    if (lowercase) {
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool category_enabled(Category cat)
{
    switch (cat) {
        case Category::Combat:    return g_settings.cat_combat;
        case Category::Movement:  return g_settings.cat_movement;
        case Category::Visuals:   return g_settings.cat_visuals;
        case Category::Misc:      return g_settings.cat_misc;
        case Category::Utilities: return g_settings.cat_misc;
        default:                  return false;
    }
}

/* @rgb_to_hsl / @hsl_to_rgb:
   round-trip HSL is what lets us derive the second gradient stop from the
   user's single colour choice. Hue lives on a wheel: shifting by ~35°
   picks a neighbour that shares saturation and lightness, so any accent
   pairs with itself — green nudges toward yellow-green, blue toward
   cyan, orange toward yellow, etc. Vape's default (blue → purple) is a
   ~30° step on the same wheel, which is why it looks harmonious. */
void rgb_to_hsl(float r, float g, float b, float& h, float& s, float& l)
{
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
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

// The two gradient stops derived from the user's single accent colour.
// stop_a is the accent itself; stop_b is the same colour shifted along the
// hue wheel and nudged slightly on lightness so the transition reads as an
// animated shine rather than a fade to grey.
void derive_gradient_stops(float in_r, float in_g, float in_b,
                           float& ar, float& ag, float& ab,
                           float& br, float& bg, float& bb)
{
    ar = in_r; ag = in_g; ab = in_b;

    float h, s, l;
    rgb_to_hsl(in_r, in_g, in_b, h, s, l);

    /* Analogous-style shimmer — small hue delta (~20°) so the pair stays
       in the same colour family and never lands on an ugly cross-primary
       combo. When the input is near-grey (s < 0.15) the hue-shift is
       skipped entirely; a hue rotation on a desaturated colour reads as a
       random tint appearing, not a shimmer. */
    if (s > 0.15f) {
        /* ~34 degrees, up from ~20. Twenty kept the pair so close that the
           sweep read as a brightness ripple rather than as colour moving across
           the words; half again is still inside the family -- blue stays blue,
           orange stays warm -- but the travel is now visibly chromatic, which
           is the whole point of a colour wave. */
        h += 0.095f;
        if (h > 1.f) h -= 1.f;
        s = std::clamp(s + 0.07f, 0.f, 1.f);
    }
    // Larger lightness delta compensates for the smaller hue shift, so
    // the two stops stay visibly distinct without leaving the family.
    l = (l < 0.5f) ? std::min(1.f, l + 0.22f) : std::max(0.f, l - 0.22f);

    hsl_to_rgb(h, s, l, br, bg, bb);
}

// Vape's gradient.fsh evaluated on the CPU at a fragment centre. Same math:
// screen-space warp, ping-pong cosine ease between the two derived stops.
ImU32 gradient_at(float frag_x, float frag_y, float time)
{
    float ar, ag, ab, br, bg, bb;
    derive_gradient_stops(g_settings.text_r, g_settings.text_g, g_settings.text_b,
                          ar, ag, ab, br, bg, bb);

    constexpr float size  = 250.f;
    /* 0.45. It was 0.28, itself a walk-back from 1.0 -- at 1.0 the ping-pong
       hit the far stop about once a second and pulled the eye off whatever you
       were reading. 0.45 is still ambient, but the sweep reads as travelling
       rather than as the whole list breathing in place. */
    constexpr float speed = 0.45f;

    /* @the sweep runs top-left to bottom-right, on a true diagonal.

       The old coord weighted x at 0.3 against y at 1.0, so the band was very
       nearly horizontal and crawled straight up the list. Equal weights make it
       45 degrees.

       The SIGNS are what set the direction, and they are not obvious. A feature
       of the pattern lives where coord + time*speed is constant, so it travels
       towards DECREASING coord. frag_y is measured from the BOTTOM of the
       framebuffer, so moving down the screen means frag_y going down. Wanting
       travel towards +x and towards screen-down therefore means coord has to
       fall as x rises and fall as the screen row descends -- and the second of
       those is already inverted by the bottom-up axis, which is why the y term
       comes out positive. Hence (frag_y - frag_x). */
    const float coord = (frag_y - frag_x) / std::max(1.f, size);
    const float warp  = 0.12f * std::sin(coord * 6.2831853f + time * 0.75f * speed);
    float f = std::fmod(coord + warp + time * speed, 1.f);
    if (f < 0.f) f += 1.f;
    const float scaled = f * 2.f;
    const int   seg = static_cast<int>(scaled);
    const float b = scaled - static_cast<float>(seg);
    const float sb = 0.5f - 0.5f * std::cos(b * 3.14159265f);

    const float lo_r = seg < 1 ? ar : br;
    const float lo_g = seg < 1 ? ag : bg;
    const float lo_b = seg < 1 ? ab : bb;
    const float hi_r = seg < 1 ? br : ar;
    const float hi_g = seg < 1 ? bg : ag;
    const float hi_b = seg < 1 ? bb : ab;

    return pack_rgba(lo_r + (hi_r - lo_r) * sb,
                     lo_g + (hi_g - lo_g) * sb,
                     lo_b + (hi_b - lo_b) * sb,
                     1.f);
}

/* @format_module_meta: the MODE, and nothing else.

   This used to print every visible Int, Float and Enum a module owned, which
   for AimAssist came out as "Regular \xC2\xB7 15 \xC2\xB7 10 \xC2\xB7 180 \xC2\xB7 5 \xC2\xB7 Chest \xC2\xB7 6" -- a
   row of numbers that is unreadable at a glance and, at speed, hard to tell
   apart from the row above it. None of it answers the only question the HUD is
   asked mid-fight, which is which way the module is currently set.

   So: the FIRST enum, and stop. Enums are the settings that name a behaviour
   ("Regular", "Players", "Hotbar"); the numbers are how it is tuned, which is
   something you read in the menu, not over someone's shoulder in a fight.
   First rather than all of them because a module declares its own Mode before
   its secondary pickers -- AimAssist's "Regular" comes before its "Chest" --
   so the first one is the headline by construction. */
std::string format_module_meta(const dt::Module* mod)
{
    if (mod == nullptr)
        return {};

    for (const dt::Setting* s : mod->settings()) {
        if (s == nullptr || !s->visible())
            continue;
        if (s->type() != dt::SettingType::Enum)
            continue;

        const auto* e  = static_cast<const dt::EnumSetting*>(s);
        const auto  sv = e->current();
        if (sv.empty())
            continue;

        return std::string{ sv };
    }
    return {};
}

/* CPU glow — Vape's stable path uses a two-ring angular sample around each
   glyph with low alpha, which reads as a soft coloured halo without needing
   a separate GL pass. Cheap because the atlas is already uploaded. */
void draw_glow(ImDrawList* dl, ImFont* font, float font_size,
               const char* text, float x, float y, ImU32 accent)
{
    constexpr int steps = 12;
    for (int ring = 1; ring <= 2; ++ring) {
        const float rad = ring * 1.15f;
        const int   alpha = ring == 1 ? 26 : 13;
        const ImU32 halo = (accent & 0x00FFFFFFu) | (alpha << IM_COL32_A_SHIFT);
        for (int i = 0; i < steps; ++i) {
            const float ang = i * (6.2831853f / steps);
            dl->AddText(font, font_size, ImVec2(x + std::cos(ang) * rad,
                                                y + std::sin(ang) * rad),
                        halo, text);
        }
    }
}

} // namespace

void request_teardown()
{
    g_pending_teardown = true;
}

/* Layout is computed once per frame in render_background() and cached here
   for render_text() to pick up. render_text() shorts-out if no valid layout
   was cached (e.g. the module is disabled or no rows survived filtering). */
struct cached_layout {
    std::vector<row>         rows;
    std::vector<al::al_rect> pixel_rects;   // bottom-left origin, framebuffer px
    float font_size   = 0.f;
    float meta_size   = 0.f;
    float meta_gap    = 0.f;
    float pad_x       = 0.f;
    float pad_y       = 0.f;
    float gap         = 0.f;
    float row_height  = 0.f;
    float right_edge  = 0.f;
    float top_edge    = 0.f;
    float bbox_x      = 0.f;   // top-left bbox in framebuffer pixels
    float bbox_y      = 0.f;
    float bbox_w      = 0.f;
    float bbox_h      = 0.f;
    int   display_h   = 0;
    bool  valid       = false;
};
static cached_layout g_layout{};

/* Optional user-imposed top-left in framebuffer pixels. When the HUD editor
   drags the arraylist, it pushes here; the layout code below reads it as the
   list's top-left corner instead of the default right-anchored placement.
   NaN means "no override — use the default anchor". */
static float g_override_x = std::numeric_limits<float>::quiet_NaN();
static float g_override_y = std::numeric_limits<float>::quiet_NaN();

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    if (g_pending_teardown) {
        g_pending_teardown = false;
        g_glass.destroy();
    }
    g_layout.valid = false;

    if (!g_enabled || display_w < 2 || display_h < 2)
        return;

    /* Refresh settings from the module. ClickGui edits the setting values
       directly via bridge::apply*, so we can't cache them at onEnable — the
       slider positions here would be stuck on whatever they were on that
       first callback. */
    if (g_settings_provider)
        g_settings_provider(g_settings);

    /* Build the dedicated atlas the first time we render — the shared dt
       atlas maxes out around 26 fb-px of glyph resolution which is why the
       arraylist looked pixelated on higher Scale settings. shared_pool bakes
       up to 96 fb-px at OversampleH=3, so a Scale=3 render (48 fb-px text)
       still samples from a downscaled atlas glyph, not an upscaled one. */
    if (!shared_pool().ensure())
        return;

    /* Base font size scales with the framebuffer height. On 1080p the default
       lands on 18 fb-px (readable but not overpowering); on 2K it lands on
       ~24; on 4K around 36. The user's Scale slider still multiplies on top,
       so someone who wants huge text can push it further. The DPI-tie
       matters because 16 fb-px on a 2K monitor is much smaller than 16 fb-px
       on 1080p, and users have been reporting that as "low quality" when
       the real culprit is the text being too small to see the antialiasing. */
    const float dpi_factor   = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float base_font_px = 18.f * dpi_factor;
    const float font_size    = std::max(6.f, base_font_px * g_settings.scale);

    ImFont* font = pick_font_for(font_size);
    if (font == nullptr)
        return;
    const bool mono          = g_settings.style == 1;
    const float gap          = 0.f;
    const float pad_x        = (mono ? 7.f : 4.f) * g_settings.scale;
    const float pad_y        = 2.f * g_settings.scale;
    const float row_height   = font_size + 2.f * pad_y;
    const float margin       = 6.f * g_settings.scale;

    /* Collect enabled Dt modules — the ArrayList itself is filtered out so
       it doesn't self-list. Metadata is left blank until Dt modules expose
       a "current mode" the same way Vape's ModeValue does. */
    std::vector<row> rows;
    rows.reserve(16);

    /* Subtext of module settings is drawn at a smaller size so it reads as
       secondary information instead of competing with the module name.
       Measure it in the same font (the pool guarantees crisp downscale
       from the big bakes to any size we ask for). */
    const float meta_size = std::max(6.f, font_size * (mono ? 1.f : 0.78f));
    const float meta_gap  = std::max(4.f, 6.f * g_settings.scale);

    for (const auto& mod_ptr : dt::ModuleManager::get().all()) {
        const dt::Module* mod = mod_ptr.get();
        if (mod == nullptr || !mod->isEnabled())
            continue;
        if (!category_enabled(mod->category()))
            continue;
        if (mod->name() == std::string_view("ArrayList"))
            continue;

        row r{};
        r.name = maybe_lower(mod->name(), g_settings.lowercase);

        r.name_width = font->CalcTextSizeA(font_size, FLT_MAX, -1.f, r.name.c_str()).x;

        if (g_settings.show_settings) {
            // Explicit module info takes precedence over the generic mode.
            // Keep it in the gray secondary text, never in the accent name.
            r.meta = mod->displayInfo();
            if (r.meta.empty())
                r.meta = format_module_meta(mod);
            if (g_settings.lowercase)
                r.meta = maybe_lower(r.meta, true);
            if (!r.meta.empty())
                r.meta_width = font->CalcTextSizeA(meta_size, FLT_MAX, -1.f, r.meta.c_str()).x;
        }

        r.width = r.name_width + (r.meta.empty() ? 0.f : meta_gap + r.meta_width);
        rows.push_back(std::move(r));
    }

    if (rows.empty())
        return;

    std::sort(rows.begin(), rows.end(),
              [](const row& a, const row& b) { return a.width > b.width; });

    /* Anchor: the default is right-anchored at margin-from-right/top. If the
       HUD editor has set an override (drag repositioning), that becomes the
       list's top-left directly. right_edge / top_edge below are still the
       coordinates the row loop uses — for the default, right_edge is the
       right side of the widest row; for the override, it's the override X
       plus the widest row's width so rows continue to right-align inside
       the moved bbox. */
    float right_edge;
    float top_edge;
    if (!std::isnan(g_override_x) && !std::isnan(g_override_y)) {
        // The widest row after sorting is index 0 — compute its own width
        // to know how far right to place `right_edge`.
        float widest = 0.f;
        for (const auto& r : rows) widest = std::max(widest, r.width + pad_x * 2.f);
        right_edge = g_override_x + widest;
        top_edge   = g_override_y;
    } else {
        right_edge = static_cast<float>(display_w) - margin;
        top_edge   = margin;
    }
    // Mono is attached to the top-right corner, even after editor repositioning.
    if (mono) {
        right_edge = static_cast<float>(display_w);
        top_edge = 0.f;
    }
    const float radius_base = (mono ? 7.f : 3.f) * g_settings.scale * g_settings.rounding;
    const float merge_threshold = 0.75f * g_settings.scale;

    std::vector<al::al_rect> pixel_rects;
    pixel_rects.reserve(rows.size());

    const int last = static_cast<int>(rows.size()) - 1;
    for (int i = 0; i <= last; ++i) {
        const float rw = rows[i].width + pad_x * 2.f;
        const float x_left = right_edge - rw;
        const float top    = top_edge + static_cast<float>(i) * (row_height + gap);
        const float cap    = std::max(0.f, std::min(rw, row_height) * 0.5f - 1.f);
        const float rad    = std::min(radius_base, cap);

        const float d_prev = (i > 0)    ? rows[i].width - rows[i - 1].width : 1e9f;
        const float d_next = (i < last) ? rows[i].width - rows[i + 1].width : 1e9f;

        const float r_tr = (i == 0)    ? rad : 0.f;
        const float r_br = (i == last) ? rad : 0.f;
        const float r_tl = (i == 0)    ? (mono ? 0.f : rad)
                                       : (d_prev > merge_threshold ? std::min(rad, d_prev) : 0.f);
        const float r_bl = (i == last) ? rad
                                       : (d_next > merge_threshold ? std::min(rad, d_next) : 0.f);

        const float y_bottom = static_cast<float>(display_h) - (top + row_height);
        pixel_rects.emplace_back(x_left, y_bottom, rw, row_height,
                                r_tl, mono ? 0.f : r_tr,
                                mono ? 0.f : r_br, r_bl);
    }

    // ── Glass background: dual-Kawase blur composited through the rects. ──
    if (g_settings.background && g_glass.available()) {
        float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -1e9f, max_y = -1e9f;
        for (const auto& r : pixel_rects) {
            min_x = std::min(min_x, r.x);
            min_y = std::min(min_y, r.y);
            max_x = std::max(max_x, r.x + r.w);
            max_y = std::max(max_y, r.y + r.h);
        }
        const int pad = static_cast<int>(std::ceil(g_settings.scale * 14.0));
        const int bx  = static_cast<int>(std::floor(min_x)) - pad;
        const int by  = static_cast<int>(std::floor(min_y)) - pad;
        const int bw  = static_cast<int>(std::ceil (max_x - min_x)) + 2 * pad;
        const int bh  = static_cast<int>(std::ceil (max_y - min_y)) + 2 * pad;

        const al::al_tint tint{ 0.f,
                               0.f,
                               0.f,
                               (mono ? 110.f : 95.f) / 255.f };
        /* edge_softness is measured in framebuffer pixels — the shader uses
           smoothstep(0, edge, dist) to feather the SDF. Vape used 0.75, which
           on a 2K display leaves less than one screen pixel of anti-aliasing
           and the rounded corners come out visibly stepped. Scaling with the
           user's Scale slider (and the DPI it already implies) keeps the
           corners as smooth on 2K as on 1080p. */
        const float aa_pixels = std::max(1.5f, 1.75f * g_settings.scale);
        g_glass.render(pixel_rects,
                       display_w, display_h,
                       bound_fb,
                       bx, by, bw, bh,
                       g_settings.blur_passes,
                       /*brightness=*/0.65f,
                       tint,
                       /*edge_softness=*/aa_pixels,
                       /*shadow_radius=*/9.f * g_settings.scale,
                       /*shadow_strength=*/0.20f,
                       /*shadow_gamma=*/1.35f);
    }

    /* Bounding box for HUD-editor hover / drag. top-left origin, framebuffer
       pixels. Derived from the pixel_rects (which are bottom-left origin)
       by inverting Y. */
    float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -1e9f, max_y = -1e9f;
    for (const auto& r : pixel_rects) {
        min_x = std::min(min_x, r.x);
        min_y = std::min(min_y, r.y);
        max_x = std::max(max_x, r.x + r.w);
        max_y = std::max(max_y, r.y + r.h);
    }
    g_layout.bbox_x = min_x;
    g_layout.bbox_y = static_cast<float>(display_h) - max_y;
    g_layout.bbox_w = max_x - min_x;
    g_layout.bbox_h = max_y - min_y;

    g_layout.rows        = std::move(rows);
    g_layout.pixel_rects = std::move(pixel_rects);
    g_layout.font_size   = font_size;
    g_layout.meta_size   = meta_size;
    g_layout.meta_gap    = meta_gap;
    g_layout.pad_x       = pad_x;
    g_layout.pad_y       = pad_y;
    g_layout.gap         = gap;
    g_layout.row_height  = row_height;
    g_layout.right_edge  = right_edge;
    g_layout.top_edge    = top_edge;
    g_layout.display_h   = display_h;
    g_layout.valid       = true;
}

void query_bbox(float& x, float& y, float& w, float& h)
{
    if (!g_layout.valid) { x = y = w = h = 0.f; return; }
    x = g_layout.bbox_x;
    y = g_layout.bbox_y;
    w = g_layout.bbox_w;
    h = g_layout.bbox_h;
}

void set_top_left(float x, float y)
{
    g_override_x = x;
    g_override_y = y;
}

float draw_editor_settings(float x, float y, float w)
{
    /* Placeholder summary — the HUD editor sub-view calls this to fill its
       right pane. For MVP it lists the current settings as read-only text;
       full editable widgets live in the ClickGui module drawer. Returns
       the pixel height consumed so the caller knows how far it drew. */
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dl == nullptr) return 0.f;

    auto& pool = shared_pool();
    if (!pool.ensure()) return 0.f;

    ImFont* label_font = pool.get(15.f);
    ImFont* value_font = pool.get(15.f);
    if (label_font == nullptr) return 0.f;

    const ImU32 label_col = IM_COL32(160, 160, 170, 255);
    const ImU32 value_col = IM_COL32(240, 240, 245, 255);

    struct row_t { const char* label; std::string value; };
    char scale_s[16];  std::snprintf(scale_s, sizeof(scale_s), "%.2fx",  g_settings.scale);
    char round_s[16];  std::snprintf(round_s, sizeof(round_s), "%.1f",   g_settings.rounding);
    char blur_s[8];    std::snprintf(blur_s, sizeof(blur_s), "%d",       g_settings.blur_passes);

    row_t rows[] = {
        { "Style",        g_settings.style == 1 ? "Mono" : "Default" },
        { "Background",   g_settings.background     ? "On" : "Off" },
        { "Glow",         g_settings.glow           ? "On" : "Off" },
        { "Gradient",     g_settings.gradient_text  ? "On" : "Off" },
        { "Show Values",  g_settings.show_settings  ? "On" : "Off" },
        { "Scale",        scale_s },
        { "Rounding",     round_s },
        { "Blur",         blur_s },
    };

    const float row_h = 26.f;
    float cursor_y = y;

    dl->PushTextureID(pool.texture_id());
    for (const auto& r : rows) {
        dl->AddText(label_font, 15.f, ImVec2(x,          cursor_y), label_col, r.label);
        // right-align the value within the pane
        const ImVec2 sz = value_font->CalcTextSizeA(15.f, FLT_MAX, -1.f, r.value.c_str());
        dl->AddText(value_font, 15.f, ImVec2(x + w - sz.x, cursor_y), value_col, r.value.c_str());
        cursor_y += row_h;
    }
    dl->PopTextureID();

    // Hint at the bottom pointing users toward the full ClickGui controls
    // for editing the values.
    cursor_y += 8.f;
    ImFont* hint_font = pool.get(13.f);
    if (hint_font != nullptr) {
        dl->PushTextureID(pool.texture_id());
        dl->AddText(hint_font, 13.f, ImVec2(x, cursor_y),
            IM_COL32(140, 140, 150, 220),
            "Drag the list on the grid to reposition.");
        dl->AddText(hint_font, 13.f, ImVec2(x, cursor_y + 18.f),
            IM_COL32(140, 140, 150, 220),
            "Edit values in the module drawer.");
        dl->PopTextureID();
    }
    cursor_y += 40.f;

    return cursor_y - y;
}

void render_text(int display_w, int display_h)
{
    if (!g_enabled || !g_layout.valid)
        return;

    /* Re-resolve the font this frame — Dt rebakes the atlas on scale
       change, and that invalidates every ImFont* we might have cached. */
    ImFont* font = pick_font_for(g_layout.font_size);
    if (font == nullptr)
        return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dl == nullptr)
        return;

    const float time = elapsed_seconds();
    const int   last = static_cast<int>(g_layout.rows.size()) - 1;

    /* Two subtle GL-side hazards that both make the text vanish:

       1) TextureId — ImDrawList::AddText asserts in debug that the current
          list's TextureId matches the font's atlas texture, but the release
          build is silent. If some earlier draw pushed another texture (SVG
          icon, emoji sheet) the glyph quads sample from that instead and
          come out invisible or garbled. Push the atlas explicitly.

       2) ClipRect — gl_hook seeds io.DisplaySize to the CANVAS size
          (1120x700) so the dt panel gets scissored inside the canvas.
          That means the foreground list opens each frame with
          ClipRect = (0,0,1120,700). Our text lives in framebuffer pixels
          (right-anchored around x = display_w - margin), which on a 2K
          display sits well past 1120 — scissored to nothing. Override the
          clip with a rect matching the framebuffer, non-intersecting, so
          the enclosing 1120x700 doesn't clamp us. */
    /* Push the pool's own atlas texture. font->ContainerAtlas points to it
       already, but ImDrawList's cmd-header carries a separate texture we
       must set explicitly — the dt atlas may have been the last thing
       bound, and glyphs would be sampled from THAT (garbage). */
    dl->PushTextureID(shared_pool().texture_id());
    dl->PushClipRect(ImVec2(0.f, 0.f),
                     ImVec2(static_cast<float>(display_w),
                            static_cast<float>(display_h)),
                     false);

    const bool mono = g_settings.style == 1;
    float ar, ag, ab, br, bg, bb;
    derive_gradient_stops(g_settings.text_r, g_settings.text_g, g_settings.text_b,
                          ar, ag, ab, br, bg, bb);
    const auto vertical_colour = [&](float t) {
        // Constant phase travels downward: t increases toward the bottom.
        // A periodic cosine keeps the wrap smooth, with no colour jump.
        t = 0.5f - 0.5f * std::cos(6.2831853f * (t - time * 0.22f));
        return pack_rgba(ar + (br - ar) * t, ag + (bg - ag) * t,
                         ab + (bb - ab) * t, g_settings.text_a);
    };
    if (mono) {
        // Sample the moving wave along the rail instead of blending only its ends.
        // Shape UVs belong to ImGui's main atlas, not the dedicated HUD font atlas.
        dl->PushTextureID(ImGui::GetIO().Fonts->TexID);
        const int segments = std::clamp(static_cast<int>(std::ceil(g_layout.bbox_h / 4.f)), 16, 256);
        const float rail_left = g_layout.right_edge - std::max(2.f, 3.f * g_settings.scale);
        for (int segment = 0; segment < segments; ++segment) {
            const float t0 = static_cast<float>(segment) / segments;
            const float t1 = static_cast<float>(segment + 1) / segments;
            const ImU32 c0 = vertical_colour(t0);
            const ImU32 c1 = vertical_colour(t1);
            dl->AddRectFilledMultiColor(
                ImVec2(rail_left, g_layout.bbox_y + t0 * g_layout.bbox_h),
                ImVec2(g_layout.right_edge, g_layout.bbox_y + t1 * g_layout.bbox_h),
                c0, c0, c1, c1);
        }
        dl->PopTextureID();
    }

    for (int i = 0; i <= last; ++i) {
        const auto& r  = g_layout.rows[i];
        const float rw = r.width + g_layout.pad_x * 2.f;
        const float x  = g_layout.right_edge - rw + g_layout.pad_x;
        const float y  = g_layout.top_edge + static_cast<float>(i) * (g_layout.row_height + g_layout.gap) + g_layout.pad_y;

        const float frag_x = x + r.name_width * 0.5f;
        const float frag_y = static_cast<float>(g_layout.display_h) - (y + g_layout.font_size * 0.5f);

        /* Gradient mode animates a two-stop screen-space fill (Vape default).
           Solid mode uses whichever colour the user picked in Settings. */
        const ImU32 accent = mono ? vertical_colour((static_cast<float>(i) + 0.5f) / static_cast<float>(last + 1)) : g_settings.gradient_text
                                 ? gradient_at(frag_x, frag_y, time)
                                 : pack_rgba(g_settings.text_r, g_settings.text_g,
                                             g_settings.text_b, g_settings.text_a);

        if (mono) {
            dl->AddText(font, g_layout.font_size,
                        ImVec2(x + g_settings.scale, y + g_settings.scale),
                        IM_COL32(0, 0, 0, 100), r.name.c_str());
        }
        if (g_settings.glow && !mono)
            draw_glow(dl, font, g_layout.font_size, r.name.c_str(), x, y, accent);
        dl->AddText(font, g_layout.font_size, ImVec2(x, y), accent, r.name.c_str());

        if (!r.meta.empty()) {
            /* Meta subtext: smaller, dim, vertically centred against the
               name's optical middle rather than sitting on the same top —
               small vs large text with a shared top edge would read as
               unaligned. Colour is a slightly transparent white so the
               subtext stays legible on any user-picked accent. */
            const float meta_y = y + (g_layout.font_size - g_layout.meta_size) * 0.5f;
            const float meta_x = x + r.name_width + g_layout.meta_gap;
            const ImU32 meta_col = mono ? IM_COL32(145, 145, 145, 255) : IM_COL32(210, 210, 220, 175);
            dl->AddText(font, g_layout.meta_size, ImVec2(meta_x, meta_y),
                        meta_col, r.meta.c_str());
        }
    }

    dl->PopClipRect();
    dl->PopTextureID();
}

} // namespace dt::modules::arraylist
