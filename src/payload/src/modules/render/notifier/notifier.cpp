#include "notifier.h"

#include "modules/render/arraylist/al_glass.h"
#include "modules/render/arraylist/arraylist.h"
#include "modules/render/arraylist/font_pool.h"

#include "vgui/bridge.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace dt::modules::notifier {

bool g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

dt::al::al_glass g_glass{};

/* #100e0c, and only ever seen with Glass off — with Glass on the frosted pane
   is the background. */
constexpr float k_bg_r = 16.f / 255.f;
constexpr float k_bg_g = 14.f / 255.f;
constexpr float k_bg_b = 12.f / 255.f;

/* @semi-transparent, but not much.

   Far enough down that the world moves behind it and it reads as part of the
   client rather than a sticker on the screen; far enough up that white text on
   it stays legible over snow, which is the case that decides the number. */
constexpr float k_bg_a = 0.88f;

struct toast
{
    std::string category;
    std::string module;
    bool enabled = false;

    /* Seconds since it arrived. Drives both the entrance and the exit. */
    float age = 0.f;

    /* Where it currently sits, and whether that has been seeded yet. Kept per
       toast rather than derived from the index so the stack SLIDES when one
       above it expires instead of snapping up a row. */
    float y = 0.f;
    bool  placed = false;
};

std::mutex g_mutex;
std::vector<toast> g_pending;

/* Owned by the render thread and touched by nothing else. */
std::vector<toast> g_live;

/* @the card the HUD editor gets when nothing is on screen.

   Not in g_live and never in it: it has no age, it is not drained, it is not
   culled, and switching the editor off makes it stop existing rather than fade.
   The Target HUD does exactly this for the same reason — you cannot place a
   thing you cannot see, and waiting for a real notification to arrive is not a
   way to position one. */
toast g_preview{ "Visuals", "Notifier", true, 0.f, 0.f, false };

/* @reset() cannot touch g_live itself, and that is the whole reason this exists.

   onDisable runs on whichever thread flipped the switch, and for the profile
   load and the unload that is not the render thread -- so touching the animated
   list there would race the draw pass mid-iteration. The request is left under
   the lock and the render thread acts on it at drain time, where it already owns
   the list. What it does then is START THE FADE rather than clear: a card that
   blinks out of existence reads as a glitch, not as a dismissal. */
bool g_clear_requested = false;

/* @the gate that stops the config load from shouting.

   Set on the first frame the overlay renders. Everything the bootstrap switches
   on happens long before that, so the toasts it would have produced are dropped
   where they are queued rather than filtered later. */
bool g_ready = false;

/* More than a handful on screen at once stops being a notification and starts
   being a wall. The oldest is sent on its way. */
constexpr std::size_t k_max_toasts = 5;

/* A backstop, not the policy. Retiring by fade leaves the retiree in the list
   for a third of a second, so a burst can briefly sit above the cap; this bounds
   how far above. */
constexpr std::size_t k_hard_cap = 16;

/* Seconds. Short: it is an acknowledgement, not a message. */
constexpr float k_fade_in = 0.18f;
constexpr float k_fade_out = 0.34f;

/* @one card, fully measured, ready to draw.

   The background pass fills these and the text pass reads them. Nothing is
   recomputed on the other side: a layout computed twice is a layout that can
   disagree with itself, and the frosted pane and the words it sits under are
   exactly the pair that must not. */
struct card
{
    /* Which toast the words come from. -1 means the editor's stand-in. */
    int source = -1;

    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    float radius = 0.f;
    float alpha = 0.f;

    float text_y = 0.f, mid_y = 0.f;
    float cat_x = 0.f, dot1_x = 0.f, mod_x = 0.f, dot2_x = 0.f, state_x = 0.f;
};

struct layout_t
{
    bool valid = false;

    float scale = 1.f;
    float dot_r = 0.f;
    float text_size = 0.f;

    /* The union of every card, for the HUD editor. */
    float bbox_x = 0.f, bbox_y = 0.f, bbox_w = 0.f, bbox_h = 0.f;

    /* @how to get from that box back to the anchor.

       The editor speaks in bounding boxes and this module is anchored by a
       centre, one row down from the box's top whenever the newest card is still
       dropping into place. Storing the offset the layout actually used makes
       set_top_left an exact inverse of it, so a click that does not move the
       cursor does not move the stack — without this, pressing on a card during
       its entrance walked the whole stack ~24 px up the screen, every time. */
    float anchor_off_x = 0.f, anchor_off_y = 0.f;

    std::vector<card> cards;
};

layout_t g_layout{};

/* @where the stack sits: a CENTRE, not a left edge.

   Cards are as wide as their own words, so the stack has no stable left edge —
   store one and the whole column slides sideways every time the widest card
   changes or expires, and the spot the user chose in the editor stops being the
   spot the next notification appears in. The centre is the thing that actually
   stays put.

   NaN until the user drags it, which is what distinguishes "never placed" from
   "deliberately placed at the default position". */
float g_anchor_cx = std::nanf("");
float g_anchor_y = std::nanf("");

ImU32 rgba(float r, float g, float b, float a)
{
    return ImGui::GetColorU32(ImVec4(r, g, b, std::clamp(a, 0.f, 1.f)));
}

ImFont* font_at(float size)
{
    auto& pool = dt::modules::arraylist::shared_pool();

    if (!pool.ensure())
        return nullptr;

    return pool.get(size);
}

/* @the background pass runs BEFORE ImGui writes this frame's delta.

   io.DeltaTime is filled in by the Win32 backend's NewFrame, which gl_hook calls
   after this pass — so what is read here is the PREVIOUS frame's. That is the
   right number anyway (it is how long the last frame took) and it is what every
   other background pass in this client animates on. Clamped so a hitch cannot
   teleport an animation and a paused frame cannot divide by zero. */
float tick_delta()
{
    return std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
}

/* Ease-out cubic: fast at the start, settling at the end. An entrance that
   decelerates reads as something arriving; a linear one reads as something being
   dragged. */
float ease_out(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    const float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

} // namespace

void push(std::string category, std::string module, bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_ready)
        return;

    toast t{};
    t.category = std::move(category);
    t.module = std::move(module);
    t.enabled = enabled;

    g_pending.push_back(std::move(t));
}

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    g_layout.valid = false;
    g_layout.cards.clear();

    if (display_w < 2 || display_h < 2)
        return;

    /* Pulled before the drain, not after, because the drain needs to know how
       long a toast lives: an overflowing stack retires its oldest by ageing it
       into the fade, and that needs the duration already in hand. */
    if (g_enabled && g_settings_provider != nullptr)
        g_settings_provider(g_settings);

    /* isfinite because a clamp cannot reject a NaN — every comparison against
       one is false, so it would sail through the slider's range and give a toast
       an age it can never reach. */
    const float life = std::isfinite(g_settings.duration)
        ? std::max(g_settings.duration, 0.2f)
        : 2.5f;

    /* Marked ready even on the frames the module is off, because "the overlay is
       alive" is a fact about the client and not about this module — otherwise
       enabling the Notifier would arm it one frame late and swallow the very
       toast announcing that it was enabled. */
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        g_ready = true;

        /* Switched off: send what is on screen out the way it was going to go
           anyway, only now. */
        if (g_clear_requested)
        {
            g_clear_requested = false;

            for (toast& t : g_live)
                t.age = std::max(t.age, life);
        }

        /* Drained under the lock, animated outside it. */
        if (!g_pending.empty())
        {
            for (toast& t : g_pending)
                g_live.insert(g_live.begin(), std::move(t));

            g_pending.clear();

            /* @the oldest is retired, not deleted.

               A resize() here would make the sixth card's arrival blink the
               first one out of existence within a single frame, which reads as a
               glitch rather than as a stack scrolling. Ageing it to the end of
               its life sends it out through the same fade every other toast
               gets. Retirees stay in the list while they leave, so k_max_toasts
               is a cap on cards at FULL strength and k_hard_cap is only there so
               a pathological burst cannot grow the list without bound. */
            for (std::size_t i = k_max_toasts; i < g_live.size(); ++i)
                g_live[i].age = std::max(g_live[i].age, life);

            if (g_live.size() > k_hard_cap)
                g_live.resize(k_hard_cap);
        }
    }

    const float delta = tick_delta();

    for (toast& t : g_live)
        t.age += delta;

    /* Ages rise with the index, so today this can only ever pop the tail. Left
       as an erase-remove anyway: the day a toast carries its own duration is the
       day an index walk starts renumbering cards mid-animation. */
    g_live.erase(
        std::remove_if(g_live.begin(), g_live.end(),
            [&](const toast& t) { return t.age >= life + k_fade_out; }),
        g_live.end());

    /* @the editor is shown a stand-in, because there is nothing to place.

       A notification is by definition not on screen most of the time, and an
       element with no bbox is invisible to the HUD editor — no handle, no label,
       no drag. So while the editor is open the stack is never empty.

       Gated on the module being ON, which every other element here does at the
       top of its own pass. Without that gate a switched-off Notifier still drew
       a card in the editor, still took a drag handle, still punched a hole in
       the grid — and, being registered last, still stole the cursor from the
       Radar underneath it. A module that is off is not on screen. */
    const bool editing = g_enabled && dt::vgui::bridge::isHudEditorActive();

    if (g_live.empty() && !editing)
        return;

    /* ── metrics ─────────────────────────────────────────────────────────── */

    /* Scaled off the framebuffer height so the card is the same physical size on
       1080p and 1440p, exactly like every other HUD element here. */
    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float scale = dpi * std::clamp(g_settings.scale, 0.5f, 2.5f);

    const float text_size = 14.5f * scale;

    ImFont* font = font_at(text_size);

    if (font == nullptr)
        return;

    const float pad_x = 15.f * scale;
    const float pad_y = 9.f * scale;
    const float gap = 9.f * scale;          /* text to dot, dot to text */
    const float dot_r = 1.8f * scale;
    const float row_gap = 7.f * scale;

    const float line_h = font->CalcTextSizeA(text_size, FLT_MAX, -1.f, "Ag").y;
    const float card_h = line_h + pad_y * 2.f;

    /* @high, but not halfway.

       Dead centre is where the crosshair is and where anything worth looking at
       in the world tends to be. A little above it clears both and still lands
       inside the cone the eye is already covering, which is the whole point of
       putting it here instead of in a corner.

       Only a default; the HUD editor overrides both axes the moment the user
       drags the stack anywhere. */
    const float anchor_cx = std::isnan(g_anchor_cx)
        ? static_cast<float>(display_w) * 0.5f
        : g_anchor_cx;

    const float anchor_y = std::isnan(g_anchor_y)
        ? static_cast<float>(display_h) * 0.34f
        : g_anchor_y;

    const float radius_base = 9.f * scale * std::clamp(g_settings.rounding, 0.f, 2.f);
    const float radius = std::max(0.f, std::min(radius_base, card_h * 0.5f - 1.f));

    g_layout.scale = scale;
    g_layout.dot_r = dot_r;
    g_layout.text_size = text_size;

    /* ── the cards ───────────────────────────────────────────────────────── */

    const std::size_t count = editing && g_live.empty() ? 1u : g_live.size();

    g_layout.cards.reserve(count);

    for (std::size_t i = 0; i < count; ++i)
    {
        const bool preview = editing && g_live.empty();
        toast& t = preview ? g_preview : g_live[i];

        const char* state = t.enabled ? "Enabled" : "Disabled";

        const ImVec2 cat_extent = font->CalcTextSizeA(text_size, FLT_MAX, -1.f, t.category.c_str());
        const ImVec2 mod_extent = font->CalcTextSizeA(text_size, FLT_MAX, -1.f, t.module.c_str());
        const ImVec2 state_extent = font->CalcTextSizeA(text_size, FLT_MAX, -1.f, state);

        /* Two dots, four gaps: category · module · state. */
        const float content_w = cat_extent.x + mod_extent.x + state_extent.x
            + gap * 4.f + dot_r * 4.f;

        const float card_w = content_w + pad_x * 2.f;

        const float target_y = anchor_y + static_cast<float>(i) * (card_h + row_gap);

        float alpha = 1.f;
        float in_t = 1.f;

        if (preview)
        {
            /* The stand-in does not animate. It is a ruler, not a notification —
               a ruler that slides into place as you look at it is a worse one. */
            t.y = target_y;
            t.placed = true;
        }
        else
        {
            /* Entrance and exit, both on the age. The entrance is skipped in
               the editor for the same reason the easing is: what is on screen
               there is a ruler. */
            in_t = editing ? 1.f : ease_out(t.age / k_fade_in);

            const float out_t = t.age <= life
                ? 1.f
                : 1.f - std::clamp((t.age - life) / k_fade_out, 0.f, 1.f);

            alpha = in_t * out_t;

            /* @arrives from above, then holds.

               Seeded a short way over its resting place rather than off the top
               of the screen: a card that flies in from the edge draws the eye to
               the travel instead of to the words, and the words are the whole
               point. After the first frame the position is EASED to its target,
               so a toast expiring above this one slides it up instead of
               teleporting it.

               Unconditional, so the stack keeps moving on the frames a card is
               too faint to draw — otherwise a card would resume its slide from
               wherever it happened to be when it faded. */
            if (!t.placed)
            {
                t.y = target_y - 22.f * scale;
                t.placed = true;
            }

            /* @no easing while the editor is open.

               A card that eases towards its target lags the cursor by about
               four frames of drag velocity on the Y axis and not at all on the
               X, so a stack dragged downwards and released settles a hundred
               pixels below the outline the user let go of. In the editor the
               stack is a thing being positioned, not a thing arriving. */
            if (editing)
                t.y = target_y;
            else
                t.y += (target_y - t.y) * std::clamp(14.f * delta, 0.f, 1.f);
        }

        if (alpha <= 0.004f)
            continue;

        card c{};
        c.source = preview ? -1 : static_cast<int>(i);
        c.w = card_w;
        c.h = card_h;
        c.radius = radius;
        c.alpha = alpha;

        /* The entrance gets its own offset on top of the easing, so the first
           moments read as a drop rather than as a fade in place. */
        c.y = t.y - (1.f - in_t) * 10.f * scale;
        c.x = anchor_cx - card_w * 0.5f;

        c.text_y = c.y + (card_h - line_h) * 0.5f;
        c.mid_y = c.y + card_h * 0.5f;

        c.cat_x = c.x + pad_x;
        c.dot1_x = c.cat_x + cat_extent.x + gap + dot_r;
        c.mod_x = c.dot1_x + dot_r + gap;
        c.dot2_x = c.mod_x + mod_extent.x + gap + dot_r;
        c.state_x = c.dot2_x + dot_r + gap;

        g_layout.cards.push_back(c);
    }

    if (g_layout.cards.empty())
        return;

    /* ── the bbox the editor grabs ───────────────────────────────────────── */

    float min_x = g_layout.cards.front().x;
    float min_y = g_layout.cards.front().y;
    float max_x = min_x + g_layout.cards.front().w;
    float max_y = min_y + g_layout.cards.front().h;

    for (const card& c : g_layout.cards)
    {
        min_x = std::min(min_x, c.x);
        min_y = std::min(min_y, c.y);
        max_x = std::max(max_x, c.x + c.w);
        max_y = std::max(max_y, c.y + c.h);
    }

    g_layout.bbox_x = min_x;
    g_layout.bbox_y = min_y;
    g_layout.bbox_w = max_x - min_x;
    g_layout.bbox_h = max_y - min_y;

    g_layout.anchor_off_x = anchor_cx - min_x;
    g_layout.anchor_off_y = anchor_y - min_y;

    g_layout.valid = true;

    /* ── the frosted panes ───────────────────────────────────────────────── */

    /* Only when the client itself is frosted. With Glass off it is a set of flat
       cards, and one acrylic surface floating among them does not read as the
       same product. */
    if (!dt::vgui::bridge::isGlassEnabled() || !g_glass.available())
        return;

    /* @grouped by opacity, because one composite carries one.

       al_glass takes a list of rects and a SINGLE opacity for all of them, and
       the cards here do not always share one: the newest can be fading in while
       the oldest fades out. Cards at the SAME opacity go in one call, which in
       the ordinary case — every card settled at 1.0 — means one call and one
       capture, exactly what the radar and the watermark each pay. Extra calls
       happen only during a transition, and last at most a third of a second.

       Grouped on the exact float rather than a rounded one: settled cards are
       all assigned the literal 1.0 by the same branch, so they compare equal
       without needing a tolerance, and rounding only ever created a bucket that
       did not match the opacity its own text was drawn at. */
    struct group
    {
        float alpha = 0.f;
        std::vector<al::al_rect> rects;
    };

    std::vector<group> groups;
    groups.reserve(g_layout.cards.size());

    for (const card& c : g_layout.cards)
    {
        /* al_glass works in framebuffer pixels with a BOTTOM-LEFT origin, where
           every ImGui coordinate here is top-left. */
        const float y_bottom = static_cast<float>(display_h) - (c.y + c.h);

        group* slot = nullptr;

        for (group& g : groups)
        {
            if (g.alpha == c.alpha)
            {
                slot = &g;
                break;
            }
        }

        if (slot == nullptr)
        {
            groups.push_back(group{ c.alpha, {} });
            slot = &groups.back();
        }

        slot->rects.emplace_back(c.x, y_bottom, c.w, c.h,
            c.radius, c.radius, c.radius, c.radius);
    }

    /* @oldest group first, so the newest pane wins an overlap.

       Index 0 is the newest card, so groups come out of the loop above
       newest-first — and each composite lands on top of the last one. Reversing
       here puts the newest pane down last, which is the same rule the text pass
       follows and the same reason: the card you just asked for is the one that
       should be on top. */
    std::reverse(groups.begin(), groups.end());

    /* @pure black at 45%, the same recipe as the rest of the client.

       The shader computes mix(blur, tint.rgb, tint.a) * brightness, so this lets
       through 0.55 * 0.72 = 40% of the blurred scene with no colour floor at all
       — which is what the arraylist, the watermark and the radar do, and what
       makes those surfaces read as glass. A dark tint COLOUR instead of black
       would add its own grey to every pixel and flatten the pane back into a
       card. */
    const al::al_tint tint{ 0.f, 0.f, 0.f, 0.45f };

    /* @one blur region for every call, not one per group.

       The region is the whole stack even when a call only paints part of it,
       because al_glass caches its framebuffers on their exact size: hand it
       three different sizes in a frame and it destroys and recreates the whole
       chain three times, every frame, and can never warm. One size per frame
       costs a little more area on the transition frames and nothing at all on
       the ones where a single group covers the stack anyway. */
    const int bbox_pad = static_cast<int>(std::ceil(scale * 16.0));

    const float stack_bottom = static_cast<float>(display_h) - (min_y + g_layout.bbox_h);

    const int bx = static_cast<int>(std::floor(min_x)) - bbox_pad;
    const int by = static_cast<int>(std::floor(stack_bottom)) - bbox_pad;
    const int bw = static_cast<int>(std::ceil(g_layout.bbox_w)) + 2 * bbox_pad;
    const int bh = static_cast<int>(std::ceil(g_layout.bbox_h)) + 2 * bbox_pad;

    for (const group& g : groups)
    {
        g_glass.render(g.rects,
            display_w, display_h,
            bound_fb,
            bx, by, bw, bh,
            arraylist::g_settings.blur_passes,
            /*brightness=*/0.72f,
            tint,
            /*edge_softness=*/std::max(1.5f, 1.75f * scale),
            /*shadow_radius=*/10.f * scale,
            /*shadow_strength=*/0.22f,
            /*shadow_gamma=*/1.35f,
            /*opacity=*/std::clamp(g.alpha, 0.f, 1.f));
    }
}

void render_text(int display_w, int display_h)
{
    if (!g_layout.valid || g_layout.cards.empty())
        return;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    if (draw_list == nullptr)
        return;

    ImFont* font = font_at(g_layout.text_size);

    if (font == nullptr)
        return;

    /* gl_hook seeds io.DisplaySize with the 1120x700 GUI canvas, so every draw
       list opens the frame clipped to that. HUD geometry is in real framebuffer
       pixels and on any display wider than 1120 the right-hand half would be
       scissored away — hence a NON-intersecting clip rect over the whole
       framebuffer. The arraylist documents the same trap. */
    draw_list->PushClipRect(ImVec2(0.f, 0.f),
        ImVec2(static_cast<float>(display_w), static_cast<float>(display_h)), false);

    float ar = 0.61f, ag = 0.29f, ab = 0.86f, aa = 1.f;
    dt::vgui::bridge::clientAccent(ar, ag, ab, aa);

    const bool glass = dt::vgui::bridge::isGlassEnabled() && g_glass.available();
    const float dot_r = g_layout.dot_r;
    const float text_size = g_layout.text_size;

    ImTextureID atlas = dt::modules::arraylist::shared_pool().texture_id();

    /* @newest LAST, which is why this counts down.

       Index 0 is the newest card and the topmost row. When one arrives, every
       card below it has its target pushed down a full row and spends a frame or
       two easing there, and during that the new card can overlap the one under
       it. Whatever is drawn last wins an overlap, and the card you just asked
       for is the one that should win it. */
    for (std::size_t n = g_layout.cards.size(); n-- > 0;)
    {
        const card& c = g_layout.cards[n];

        const toast& t = c.source < 0
            ? g_preview
            : g_live[static_cast<std::size_t>(c.source)];

        const char* state = t.enabled ? "Enabled" : "Disabled";
        const float alpha = c.alpha;

        /* ── the card ────────────────────────────────────────────────────── */

        /* With Glass on the pane was already composited by the shader pass, and
           painting the flat card over it would bury the blur under the very
           colour it was meant to replace. */
        if (!glass)
        {
            draw_list->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.x + c.w, c.y + c.h),
                rgba(k_bg_r, k_bg_g, k_bg_b, k_bg_a * alpha), c.radius);
        }

        draw_list->AddRect(ImVec2(c.x, c.y), ImVec2(c.x + c.w, c.y + c.h),
            rgba(1.f, 1.f, 1.f, 0.09f * alpha), c.radius, 0,
            std::max(1.f, g_layout.scale * 0.9f));

        /* ── the separators ──────────────────────────────────────────────── */

        /* Drawn BEFORE the atlas is pushed. A filled shape samples the shared
           white-pixel UV, which indexes ImGui's default atlas; under a font-pool
           texture push that UV lands on an unrelated texel and the shape comes
           out invisible. The watermark lost its separator line to exactly this,
           and these are separators too. */

        /* Quiet on purpose: they are punctuation, and punctuation you notice is
           punctuation doing too much. */
        const ImU32 dot_col = rgba(1.f, 1.f, 1.f, 0.20f * alpha);

        draw_list->AddCircleFilled(ImVec2(c.dot1_x, c.mid_y), dot_r, dot_col, 8);
        draw_list->AddCircleFilled(ImVec2(c.dot2_x, c.mid_y), dot_r, dot_col, 8);

        /* ── the words ───────────────────────────────────────────────────── */

        draw_list->PushTextureID(atlas);

        /* The category is context, so it sits back. */
        draw_list->AddText(font, text_size, ImVec2(c.cat_x, c.text_y),
            rgba(1.f, 1.f, 1.f, 0.42f * alpha), t.category.c_str());

        /* The module is the subject, and it wears the client's accent. */
        draw_list->AddText(font, text_size, ImVec2(c.mod_x, c.text_y),
            rgba(ar, ag, ab, alpha), t.module.c_str());

        /* What happened to it — the thing you actually came to read. */
        draw_list->AddText(font, text_size, ImVec2(c.state_x, c.text_y),
            rgba(1.f, 1.f, 1.f, 0.88f * alpha), state);

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

    x = g_layout.bbox_x;
    y = g_layout.bbox_y;
    w = g_layout.bbox_w;
    h = g_layout.bbox_h;
}

void set_top_left(float x, float y)
{
    if (!g_layout.valid)
        return;

    /* The editor speaks in bounding-box corners; the stack is anchored by a
       centre, and by a top edge the newest card may still be falling towards.
       The layout recorded the exact offset it used this frame, so this is its
       inverse rather than an approximation of it — drop the stack where the
       outline is and it stays there, and a click that moves the cursor nowhere
       moves the stack nowhere. */
    g_anchor_cx = x + g_layout.anchor_off_x;
    g_anchor_y = y + g_layout.anchor_off_y;
}

void reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_pending.clear();
    g_clear_requested = true;
}

} // namespace dt::modules::notifier
