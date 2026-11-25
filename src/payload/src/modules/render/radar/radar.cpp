#include "radar.h"

#include "radar_source.h"

#include "modules/render/arraylist/al_glass.h"
#include "modules/render/arraylist/arraylist.h"
#include "modules/render/arraylist/font_pool.h"

#include "vgui/bridge.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace dt::modules::radar {

bool g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

dt::al::al_glass g_glass{};

constexpr float k_pi = 3.14159265358979323846f;

/* The card's own colour, no longer a setting. #0e0e0e, and only ever seen with
   Glass off — with Glass on the frosted pane is the background. */
constexpr float k_bg = 14.f / 255.f;

struct layout_t
{
    bool valid = false;

    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    float radius = 0.f;
    float scale = 1.f;

    /* The header strip along the top: title on the left, player count on the
       right. */
    float header_y = 0.f, header_h = 0.f;

    /* Centre of the plot and the radius the range maps onto, in the same
       framebuffer pixels as x/y. */
    float cx = 0.f, cy = 0.f, dial = 0.f;
};

layout_t g_layout{};

/* @where the panel sits.

   NaN until the user drags it, which is what distinguishes "never placed" from
   "deliberately placed at the default position" — the two need different answers
   when the framebuffer changes size. */
float g_anchor_x = std::nanf("");
float g_anchor_y = std::nanf("");

/* Eased in and out so enabling the module is not a rectangle appearing on one
   frame. */
float g_reveal = 0.f;

/* @the sweep, in degrees clockwise from straight ahead.

   Advanced in the background pass because that is the half of the frame that has
   a delta, and kept across the fade so switching the module off and on does not
   restart the scan from the top. */
float g_sweep = 0.f;

float tick_delta()
{
    const float dt = ImGui::GetIO().DeltaTime;

    /* A hitch must not teleport an animation; a paused frame must not divide by
       zero further down. */
    return std::clamp(dt, 0.0001f, 0.1f);
}

float approach(float current, float target, float rate, float delta)
{
    return current + (target - current) * std::clamp(rate * delta, 0.f, 1.f);
}

ImU32 rgba(float r, float g, float b, float a)
{
    return ImGui::GetColorU32(ImVec4(r, g, b, std::clamp(a, 0.f, 1.f)));
}

/* Screen direction for a bearing in degrees, where 0 is straight up (ahead) and
   positive turns clockwise — the same convention the blips are placed with. */
ImVec2 heading(float degrees)
{
    const float rad = degrees * (k_pi / 180.f);
    return ImVec2(std::sin(rad), -std::cos(rad));
}

ImFont* font_at(float size)
{
    auto& pool = dt::modules::arraylist::shared_pool();

    if (!pool.ensure())
        return nullptr;

    return pool.get(size);
}

} // namespace

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    g_layout.valid = false;

    if (display_w < 2 || display_h < 2)
        return;

    const float delta = tick_delta();

    /* @55 degrees a second, always turning.

       Advanced whether or not anything is drawn, so the sweep does not jump back
       to the top every time the panel fades in. Slow enough to read as a scan
       rather than a spinner: a full revolution takes about six and a half
       seconds. */
    g_sweep += delta * 55.f;

    if (g_sweep >= 360.f)
        g_sweep -= std::floor(g_sweep / 360.f) * 360.f;

    if (!g_enabled)
    {
        g_reveal = approach(g_reveal, 0.f, 12.f, delta);

        if (g_reveal < 0.004f)
        {
            g_reveal = 0.f;
            return;
        }
    }
    else
    {
        if (g_settings_provider != nullptr)
            g_settings_provider(g_settings);

        g_reveal = approach(g_reveal, 1.f, 12.f, delta);

        /* Polled here rather than in the draw pass: this is the half of the frame
           that is allowed to touch the JVM, and it runs before ImGui exists, so
           the draw pass gets a finished list of plain floats. */
        poll(std::max(g_settings.range, 1.f));
    }

    const float alpha = std::clamp(g_reveal, 0.f, 1.f);

    if (alpha <= 0.004f)
        return;

    /* ── metrics ─────────────────────────────────────────────────────────── */

    /* Scaled off the framebuffer height so the dial is the same physical size on
       1080p and 1440p — every other HUD element in this client does this, and a
       HUD that is half the size on a bigger monitor is the bug it prevents. */
    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float scale = dpi * std::clamp(g_settings.scale, 0.5f, 2.5f);

    const float pad = 11.f * scale;
    const float header_h = 19.f * scale;
    const float dial = 62.f * scale;

    const float w = dial * 2.f + pad * 2.f;
    const float h = pad + header_h + dial * 2.f + pad;

    /* ── placement ───────────────────────────────────────────────────────── */

    float x = 0.f;
    float y = 0.f;

    if (!std::isnan(g_anchor_x) && !std::isnan(g_anchor_y))
    {
        x = g_anchor_x;
        y = g_anchor_y;
    }
    else
    {
        /* @bottom right, and clear of everything that already lives there.

           The top-right is the arraylist's, the top-left the watermark's, the
           middle-right the Target HUD's, and the bottom-centre belongs to the
           hotbar, the health rows and the chat. The bottom-right corner is the
           one part of the screen no other element in this client claims.

           Still only a default; the HUD editor overrides it the moment the user
           drags the panel anywhere. */
        x = static_cast<float>(display_w) - w - 24.f * dpi;
        y = static_cast<float>(display_h) - h - 24.f * dpi;
    }

    const float radius_base = 8.f * scale * std::clamp(g_settings.rounding, 0.f, 2.f);
    const float radius = std::min(radius_base, std::max(0.f, std::min(w, h) * 0.5f - 1.f));

    g_layout.valid = true;
    g_layout.x = x;
    g_layout.y = y;
    g_layout.w = w;
    g_layout.h = h;
    g_layout.radius = radius;
    g_layout.scale = scale;
    g_layout.header_y = y + pad * 0.4f;
    g_layout.header_h = header_h;
    g_layout.cx = x + w * 0.5f;
    g_layout.cy = y + pad + header_h + dial;
    g_layout.dial = dial;

    /* ── the frosted pane ────────────────────────────────────────────────── */

    /* Only when the panel itself is frosted. With Glass off the client is a set
       of flat cards, and one acrylic surface floating among them does not read as
       the same product. */
    if (!dt::vgui::bridge::isGlassEnabled() || !g_glass.available())
        return;

    /* al_glass works in framebuffer pixels with a BOTTOM-LEFT origin, where every
       ImGui coordinate here is top-left. */
    const float y_bottom = static_cast<float>(display_h) - (y + h);

    std::vector<al::al_rect> rects;
    rects.emplace_back(x, y_bottom, w, h, radius, radius, radius, radius);

    const int bbox_pad = static_cast<int>(std::ceil(scale * 16.0));
    const int bx = static_cast<int>(std::floor(x)) - bbox_pad;
    const int by = static_cast<int>(std::floor(y_bottom)) - bbox_pad;
    const int bw = static_cast<int>(std::ceil(w)) + 2 * bbox_pad;
    const int bh = static_cast<int>(std::ceil(h)) + 2 * bbox_pad;

    /* @pure black at 45%, the same recipe as the rest of the client.

       The shader computes mix(blur, tint.rgb, tint.a) * brightness, so this lets
       through 0.55 * 0.72 = 40% of the blurred scene with no colour floor at all
       — which is what the arraylist and the watermark do, and what makes those
       surfaces read as glass. A dark tint COLOUR instead of black would add its
       own grey to every pixel and flatten the pane back into a card. */
    const al::al_tint tint{ 0.f, 0.f, 0.f, 0.45f };

    g_glass.render(rects,
        display_w, display_h,
        bound_fb,
        bx, by, bw, bh,
        std::max(arraylist::g_settings.blur_passes, 10),
        /*brightness=*/0.72f,
        tint,
        /*edge_softness=*/std::max(1.5f, 1.75f * scale),
        /*shadow_radius=*/10.f * scale,
        /*shadow_strength=*/0.22f,
        /*shadow_gamma=*/1.35f,
        /*opacity=*/alpha);
}

void render_text(int display_w, int display_h)
{
    if (!g_layout.valid)
        return;

    const float alpha = std::clamp(g_reveal, 0.f, 1.f);

    if (alpha <= 0.004f)
        return;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    if (draw_list == nullptr)
        return;

    /* gl_hook seeds io.DisplaySize with the 1120x700 GUI canvas, so every draw
       list opens the frame clipped to that. HUD geometry is in real framebuffer
       pixels and on any display wider than 1120 the right-hand half would be
       scissored away — hence a NON-intersecting clip rect covering the whole
       framebuffer. The arraylist documents the same trap. */
    draw_list->PushClipRect(ImVec2(0.f, 0.f),
        ImVec2(static_cast<float>(display_w), static_cast<float>(display_h)), false);

    const float scale = g_layout.scale;
    const float cx = g_layout.cx;
    const float cy = g_layout.cy;
    const float dial = g_layout.dial;
    const float hair = std::max(1.f, scale * 0.75f);

    /* ── the card ────────────────────────────────────────────────────────── */

    /* With Glass on the pane was already composited by the shader pass, and
       painting the flat card over it would bury the blur under the very colour it
       was meant to replace. */
    if (!dt::vgui::bridge::isGlassEnabled() || !g_glass.available())
    {
        draw_list->AddRectFilled(
            ImVec2(g_layout.x, g_layout.y),
            ImVec2(g_layout.x + g_layout.w, g_layout.y + g_layout.h),
            rgba(k_bg, k_bg, k_bg, alpha),
            g_layout.radius);
    }

    /* The lit rim. More than the blur behind it, this is what makes a surface
       read as a pane rather than as a hole cut in the screen. */
    draw_list->AddRect(
        ImVec2(g_layout.x, g_layout.y),
        ImVec2(g_layout.x + g_layout.w, g_layout.y + g_layout.h),
        rgba(1.f, 1.f, 1.f, 0.10f * alpha),
        g_layout.radius, 0, std::max(1.f, scale * 0.9f));

    /* ── the plot ────────────────────────────────────────────────────────── */

    /* Sunk a shade below the card it sits in. Without it the rings float in the
       middle of a panel with no boundary of their own, and the whole thing reads
       as decoration rather than as an instrument. */
    draw_list->AddCircleFilled(ImVec2(cx, cy), dial,
        rgba(0.f, 0.f, 0.f, 0.34f * alpha), 96);

    /* ── the sweep ───────────────────────────────────────────────────────── */

    /* @one fan, not forty triangles -- and that is the whole fix.

       The old version drew each slice of the wedge with its own AddTriangleFilled.
       ImGui antialiases every filled shape by feathering its outline, so each
       slice arrived with a soft edge down BOTH of its radial sides, and where two
       neighbours met their two feathered edges stacked into a visible seam. Forty
       slices, thirty-nine seams: a hatched fan of lines rather than a sweep. The
       alpha was never the problem, and no amount of tuning it would have helped.

       Written as a single indexed triangle fan instead. The slices SHARE their
       rim vertices, so there is no interior edge left to feather and the colour
       is interpolated across the surface by the rasteriser — a real gradient,
       with nothing to band.

       Also gone: the bright leading-edge line and the arc where it met the rim.
       Both were literally lines, both were the loudest thing on a panel whose job
       is showing where people are, and the gradient already says which way the
       thing is turning. */
    {
        constexpr int   segs = 64;
        constexpr float trail = 84.f;

        /* Taken from the atlas rather than from the draw list's shared data, which
           is an opaque type without imgui_internal.h. Same value: this fan is
           drawn outside any font-pool texture push, so the list is still bound to
           the default atlas that owns this UV. */
        const ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;

        /* Out to the rim exactly, so the fan's one hard edge — the outer arc,
           which carries no feathering now — lands under the ring drawn over it. */
        const float reach = dial;

        draw_list->PrimReserve(segs * 3, segs + 2);

        const unsigned int base = draw_list->_VtxCurrentIdx;

        /* The hub is dim on purpose. It is shared by every triangle, so it can
           only hold one alpha, and putting the brightness out at the rim is both
           what a real sweep looks like and what keeps the centre dot readable. */
        draw_list->PrimWriteVtx(ImVec2(cx, cy), uv,
            rgba(1.f, 1.f, 1.f, 0.035f * alpha));

        for (int i = 0; i <= segs; ++i)
        {
            const float t = 1.f - static_cast<float>(i) / static_cast<float>(segs);
            const ImVec2 d = heading(g_sweep - trail * static_cast<float>(i) / segs);

            draw_list->PrimWriteVtx(
                ImVec2(cx + d.x * reach, cy + d.y * reach), uv,
                rgba(1.f, 1.f, 1.f, 0.17f * t * t * alpha));
        }

        for (int i = 0; i < segs; ++i)
        {
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2 + i));
        }
    }

    /* @a vignette, so the plot has depth instead of being a flat hole.

       Eight rings of thickening black from 80% of the radius out to the rim. The
       eye reads the darkening as curvature, which is what stops a circle of flat
       colour from looking like a sticker on the card. */
    for (int i = 0; i < 8; ++i)
    {
        const float t = static_cast<float>(i) / 7.f;
        const float rr = dial * (0.80f + 0.20f * t);

        draw_list->AddCircle(ImVec2(cx, cy), rr,
            rgba(0.f, 0.f, 0.f, 0.055f * t * alpha), 64,
            std::max(1.f, dial * 0.035f));
    }

    /* Cross hairs, drawn under the rings so the rings cut them cleanly. */
    {
        const ImU32 hair_col = rgba(1.f, 1.f, 1.f, 0.07f * alpha);

        draw_list->AddLine(ImVec2(cx, cy - dial), ImVec2(cx, cy + dial), hair_col, hair);
        draw_list->AddLine(ImVec2(cx - dial, cy), ImVec2(cx + dial, cy), hair_col, hair);
    }

    /* Two rings: the outer is the range, the inner is half of it, so a blip's
       distance can be read at a glance instead of estimated. */
    draw_list->AddCircle(ImVec2(cx, cy), dial,
        rgba(1.f, 1.f, 1.f, 0.15f * alpha), 96, hair);

    draw_list->AddCircle(ImVec2(cx, cy), dial * 0.5f,
        rgba(1.f, 1.f, 1.f, 0.09f * alpha), 72, hair);

    /* ── the blips ───────────────────────────────────────────────────────── */

    const float range = std::max(g_settings.range, 1.f);
    const marker mode = static_cast<marker>(g_settings.mode);

    /* Drawn far-to-near so a close player is never hidden under a distant one.
       The source list is sorted nearest-first, so this walks it backwards. */
    const std::vector<blip>& blips = latest();

    /* What the header actually counts. Not blips.size(): a player sitting exactly
       at the range edge fades to nothing and is skipped below, and a header that
       says 3 over two visible dots is a header nobody trusts again. */
    int shown = 0;

    for (std::size_t i = blips.size(); i-- > 0; )
    {
        const blip& b = blips[i];

        /* @Breeze's fade, unchanged: solid inside ten blocks, linear to nothing
           at the range setting. The max() on the span is what keeps the division
           alive at the setting's own minimum of ten, where Breeze divides by zero
           and renders nothing at all. */
        const float span = std::max(1.f, range - 10.f);
        const float fade = 1.f - std::clamp(std::max(0.f, b.distance - 10.f) / span, 0.f, 1.f);

        if (fade <= 0.01f)
            continue;

        ++shown;

        /* Ground distance, not the 3-D one. On a top-down dial the radius can
           only mean "how far away across the map"; feeding it the 3-D distance
           pushes anyone above or below you outward for a reason the dial has no
           way to show. The filter and the fade above stay on the 3-D distance,
           which is the one Breeze filters and fades on. */
        const float radial = std::clamp(b.ground / range, 0.f, 1.f) * dial;

        const ImVec2 dir = heading(b.bearing);
        const float px = cx + dir.x * radial;
        const float py = cy + dir.y * radial;

        float r = g_settings.blip_r;
        float g = g_settings.blip_g;
        float bl = g_settings.blip_b;

        if (g_settings.show_height)
        {
            /* A top-down dial cannot show height, so height is folded into the
               colour: above you washes toward white, below you toward black.
               Saturating at four blocks, roughly two player heights — past that
               the exact number stops mattering and only the direction does. */
            const float lift = std::clamp(b.height / 4.f, -1.f, 1.f);

            if (lift > 0.f)
            {
                r += (1.f - r) * lift * 0.55f;
                g += (1.f - g) * lift * 0.55f;
                bl += (1.f - bl) * lift * 0.55f;
            }
            else
            {
                const float k = 1.f + lift * 0.45f;
                r *= k; g *= k; bl *= k;
            }
        }

        /* @the ping: the sweep lights a blip as it crosses it, then lets it cool.

           This is the behaviour that makes a radar read as a radar rather than as
           a plot of dots with a decoration spinning over them. The contact is the
           moment the sweep line passes the blip's bearing, so the age of that
           contact is simply how far the sweep has travelled since -- measured
           the same way round the dial that the sweep turns.

           Fading over 110 degrees, which at 55 deg/s is a two-second afterglow:
           long enough that a blip is never fully dark between passes, short
           enough that the pass is still an event you can see happen. Squared, so
           the flash is close behind the line and the decay is gentle. */
        float since = std::fmod(g_sweep - b.bearing, 360.f);

        if (since < 0.f)
            since += 360.f;

        const float ping_linear = 1.f - std::clamp(since / 110.f, 0.f, 1.f);
        const float ping = ping_linear * ping_linear;

        /* The colour's own alpha counts. The ClickGui gives this setting a live
           alpha rail; dropping it here meant the user could drag that rail all
           day, watch it persist to the profile, and never see it do anything. */
        const float base_a = g_settings.blip_a * fade * alpha;

        /* Never below 55%: a blip you cannot see between passes is a radar that
           hides players for two seconds at a time, which is worse than one with
           no effect at all. The ping adds on top of that floor, it does not
           replace it. */
        const float lit = base_a * (0.55f + 0.45f * ping);

        const float size = 4.6f * scale;

        /* @the halo, and it is the whole look.

           Two soft rings under the marker in its own colour, swelling with the
           ping. Without them the blips are flat shapes sitting on the dial; with
           them they are lights ON it, and the sweep visibly blows on each one as
           it goes past. */
        {
            const float glow = size * (1.7f + 1.5f * ping);

            draw_list->AddCircleFilled(ImVec2(px, py), glow,
                rgba(r, g, bl, base_a * 0.10f * (0.35f + ping)), 20);

            draw_list->AddCircleFilled(ImVec2(px, py), glow * 0.55f,
                rgba(r, g, bl, base_a * 0.16f * (0.35f + ping)), 20);
        }

        const ImU32 col = rgba(r, g, bl, lit);

        if (mode == marker::dot)
        {
            draw_list->AddCircleFilled(ImVec2(px, py), size * 0.72f, col, 16);
            continue;
        }

        /* Breeze's ARROW, in two dimensions: a triangle pointing away from the
           centre, so the blip carries its own bearing even when it is sitting on
           the rim among others. */
        const float side_x = -dir.y;
        const float side_y = dir.x;

        const ImVec2 tip(px + dir.x * size, py + dir.y * size);
        const ImVec2 left(px - dir.x * size * 0.62f + side_x * size * 0.62f,
                          py - dir.y * size * 0.62f + side_y * size * 0.62f);
        const ImVec2 right(px - dir.x * size * 0.62f - side_x * size * 0.62f,
                           py - dir.y * size * 0.62f - side_y * size * 0.62f);

        draw_list->AddTriangleFilled(tip, left, right, col);
    }

    /* You, at the centre, drawn over everything so a blip sitting on top of you
       cannot swallow the one fixed reference point on the dial. */
    draw_list->AddCircleFilled(ImVec2(cx, cy), std::max(1.5f, 2.2f * scale),
        rgba(1.f, 1.f, 1.f, 0.75f * alpha), 12);

    /* ── text, last ──────────────────────────────────────────────────────── */

    /* The header used to open with a little accent ring and needle -- a miniature
       of the instrument below it. Removed on request, and it was the right call:
       a panel that already IS a radar does not need a picture of one, and the
       mark was the only thing on the card competing with the dial for attention. */
    const float text_left = g_layout.x + 11.f * scale;
    const float header_mid = g_layout.header_y + g_layout.header_h * 0.5f;

    /* @every filled shape above, then all the text.

       A filled rect samples the shared white-pixel UV, which indexes ImGui's
       DEFAULT atlas. Drawing one inside a font-pool texture push lands that UV on
       an unrelated texel and the shape comes out invisible — the watermark lost
       its separator line to exactly this. So the shapes finish first and the
       atlas is pushed once, around nothing but AddText. */
    const float title_size = 12.5f * scale;
    const float count_size = 11.5f * scale;
    const float north_size = 10.f * scale;

    ImFont* title_font = font_at(title_size);
    ImFont* small_font = font_at(count_size);

    if (title_font != nullptr && small_font != nullptr)
    {
        char count_text[16];
        std::snprintf(count_text, sizeof(count_text), "%d", shown);

        const ImVec2 title_extent = title_font->CalcTextSizeA(title_size, FLT_MAX, -1.f, "Radar");
        const ImVec2 count_extent = small_font->CalcTextSizeA(count_size, FLT_MAX, -1.f, count_text);
        draw_list->PushTextureID(dt::modules::arraylist::shared_pool().texture_id());

        draw_list->AddText(title_font, title_size,
            ImVec2(text_left, header_mid - title_extent.y * 0.5f),
            rgba(1.f, 1.f, 1.f, 0.94f * alpha), "Radar");

        /* Grey, and deliberately quiet: it is a count, not a headline. */
        draw_list->AddText(small_font, count_size,
            ImVec2(g_layout.x + g_layout.w - 11.f * scale - count_extent.x,
                   header_mid - count_extent.y * 0.5f),
            rgba(1.f, 1.f, 1.f, 0.38f * alpha), count_text);

        /* @all four, and they MOVE.

           The dial is view-up — straight up is always straight ahead — so nothing
           on it is fixed to the world, and a single letter nailed to the top
           would be a compass that always reads the same. The four letters ride
           the rim instead: whichever one is at the top is the direction you are
           facing, and the rest tell you where the others went.

           Minecraft's yaw has SOUTH at zero, which is the trap the previous
           version fell into: it assumed north was zero and put the N on the south
           side of the dial for as long as it existed. South 0, west 90, north
           180, east 270 — and the relative bearing of each is that value minus
           the viewer's yaw, because the bearings the blips use are in the very
           same convention. */
        const float yaw = viewer_yaw();

        struct cardinal { const char* label; float absolute; };

        static constexpr cardinal points[] = {
            { "N", 180.f }, { "E", 270.f }, { "S", 0.f }, { "W", 90.f },
        };

        for (const cardinal& point : points)
        {
            float bearing = std::fmod(point.absolute - yaw, 360.f);

            if (bearing < 0.f)
                bearing += 360.f;

            const ImVec2 extent =
                small_font->CalcTextSizeA(north_size, FLT_MAX, -1.f, point.label);

            const ImVec2 d = heading(bearing);
            const float ring = dial - extent.y * 0.5f - 5.f * scale;

            /* North reads a shade brighter than the other three. It is the one
               people actually navigate by, and four letters at one weight is a
               ring of noise you have to stop and read. */
            const float weight = point.absolute == 180.f ? 0.55f : 0.32f;

            draw_list->AddText(small_font, north_size,
                ImVec2(cx + d.x * ring - extent.x * 0.5f,
                       cy + d.y * ring - extent.y * 0.5f),
                rgba(1.f, 1.f, 1.f, weight * alpha), point.label);
        }

        draw_list->PopTextureID();
    }

    draw_list->PopClipRect();
}

void query_bbox(float& x, float& y, float& w, float& h)
{
    if (!g_layout.valid)
    {
        x = y = w = h = 0.f;
        return;
    }

    x = g_layout.x;
    y = g_layout.y;
    w = g_layout.w;
    h = g_layout.h;
}

void set_top_left(float x, float y)
{
    g_anchor_x = x;
    g_anchor_y = y;
}

void reset()
{
    /* @the world only. The panel's own fade is deliberately left alone.

       Zeroing the reveal here looked tidy and did two bad things. It made the
       ease-out in render_background dead code, so switching the module off popped
       the dial away instead of fading it; and because onDisable runs from the
       ClickGui in the middle of a frame — after the background pass has already
       composited the pane and before the text pass draws into it — it also left
       one frame of an empty frosted square on screen with Glass on.

       Clearing the blips is enough: the dial fades out empty, which is the truth,
       and re-enabling starts from wherever the fade got to. */
    reset_source();
}

} // namespace dt::modules::radar
