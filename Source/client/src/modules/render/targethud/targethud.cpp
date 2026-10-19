#include "targethud.h"

#include "head_cache.h"
#include "hud_font.h"
#include "target_source.h"

#include "modules/render/arraylist/al_glass.h"
#include "modules/render/arraylist/arraylist.h"
#include "modules/render/arraylist/font_pool.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"

#include "core/profile/PlayerProfile.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vanta::modules::targethud {

bool g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

namespace hf = vanta::modules::hitfx;
namespace cap = vanta::modules::hitfx::capture;

vanta::al::al_glass g_glass{};

struct layout_t
{
    bool valid = false;

    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    float radius = 0.f;
    float alpha = 0.f;
    float scale = 1.f;

    /* everything below is in the same framebuffer pixels as x/y */
    float head_x = 0.f, head_y = 0.f, head_size = 0.f;
    float column_x = 0.f, column_w = 0.f;
    float name_y = 0.f, name_size = 0.f;
    float value_y = 0.f, value_size = 0.f;
    float bar_x = 0.f, bar_y = 0.f, bar_w = 0.f, bar_h = 0.f;
};

layout_t g_layout{};

/* @where the panel sits when it is not following.

   NaN means "never placed", which is how the HUD editor's other two elements
   distinguish a default anchor from a dragged one. */
float g_anchor_x = std::nanf("");
float g_anchor_y = std::nanf("");

/* @the smoothed screen position used by Follow mode.

   The projected point jumps: the target strafes, the camera turns, and every
   frame the projection lands somewhere new. Following it raw is the
   "teleporting" a follow HUD is judged on. This is the value that is actually
   drawn, eased toward the projection, and it is also what makes a target SWITCH
   read as the panel travelling rather than cutting. */
float g_follow_x = 0.f;
float g_follow_y = 0.f;
bool  g_follow_seeded = false;

/* @the next good projection SNAPS instead of easing.

   Set when the target leaves the view. Clearing g_follow_seeded to get the same
   effect is what broke the freeze: `frozen` below is derived from that same flag,
   so unseeding it on the first offscreen frame ended the hold after exactly one
   frame and the panel teleported to the anchor for the rest of the fade -- the
   very symptom the hold exists to remove. The two facts are separate, so they get
   separate flags. */
bool g_follow_resnap = false;

/* Hysteresis for the edge test. Leaving the view needs the target to be properly
   outside it, not a pixel outside: without that, a target strafing along the
   border flips placed/offscreen every frame and the panel strobes between the
   player and the anchor several times a second. */
bool g_follow_onscreen = false;

/* @how much of the panel the follow placement is currently allowing.

   Follow binds the panel to a player. When that player cannot be projected --
   behind you, or off the edge of the screen -- there is no honest place to put
   the panel, and the two dishonest ones are what the user reported: clamp it to
   the screen edge and it sits there detached from anybody, or fall back to the
   fixed anchor and it teleports across the screen and looks stuck. It fades out
   instead, and fades back in on the player when they return. */
float g_follow_vis = 1.f;

/* @the smoothed WORLD point, which is the half that was missing.

   Smoothing only the projected point makes the panel lag behind the player every
   time the CAMERA moves — turn your view and the whole projection jumps, the
   easing drags the panel across the screen after it, and what you see is a panel
   sliding around rather than one attached to anybody. Smoothing the world point
   instead removes the target's own jitter while leaving camera motion
   instantaneous, which is correct: when you turn, the player moves on screen
   immediately, so the panel must too. */
double g_world_x = 0.0;
double g_world_y = 0.0;
double g_world_z = 0.0;
bool   g_world_seeded = false;

/* Health has its own easing. A hit takes several hearts off in one tick, and a
   bar that snaps is a bar you cannot read — the drain is the part that tells you
   how hard you hit them. */
float g_shown_health = 0.f;
bool  g_health_seeded = false;

/* who the eased values above belong to */
std::string g_shown_name{};

/* @how far the name's gradient has travelled, in cycles.

   Kept as a wrapped fraction rather than an ever-growing seconds counter: a
   float that has been accumulating for an hour has lost the precision that the
   fractional part is entirely made of, and the sweep starts to stutter. */
float g_sweep = 0.f;

/* @what is on screen this frame, which is not always what the poll found.

   render_text used to read target_source::latest() for itself. That is fine
   until the panel has to draw something the poll never produced — the HUD
   editor's stand-in — at which point the two passes are looking at different
   objects and lay out different panels. One snapshot, filled by the layout pass,
   read by the draw pass. */
target_snapshot g_drawn{};

std::chrono::steady_clock::time_point g_last_tick{};
bool g_clock_seeded = false;

float tick_delta()
{
    const auto now = std::chrono::steady_clock::now();

    if (!g_clock_seeded)
    {
        g_clock_seeded = true;
        g_last_tick = now;
        return 1.f / 60.f;
    }

    const float delta = std::chrono::duration<float>(now - g_last_tick).count();
    g_last_tick = now;

    /* Clamped because this pass also runs on the frame after a world load or an
       alt-tab, where the real delta is seconds and every eased value would jump
       straight to its target — which is the exact artefact the easing exists to
       prevent. */
    return std::clamp(delta, 0.f, 0.1f);
}

float approach(float current, float target, float rate, float delta)
{
    return current + (target - current) * std::clamp(rate * delta, 0.f, 1.f);
}

/* @easing by half-life rather than by rate.

   `half_life` is the time it takes to close half the remaining gap, and pow()
   makes that exactly true at any frame rate — a rate-times-delta lerp only
   approximates it and drifts as the frame time changes, so the same setting
   feels different at 60 and at 240fps. It also cannot overshoot, which a lerp
   whose rate*delta exceeds 1 does. */
double decay(double current, double target, double half_life, double delta)
{
    if (half_life <= 0.0)
        return target;

    const double keep = std::pow(0.5, delta / half_life);

    return target + (current - target) * keep;
}

ImU32 rgba(float r, float g, float b, float a)
{
    return IM_COL32(
        static_cast<int>(std::clamp(r, 0.f, 1.f) * 255.f),
        static_cast<int>(std::clamp(g, 0.f, 1.f) * 255.f),
        static_cast<int>(std::clamp(b, 0.f, 1.f) * 255.f),
        static_cast<int>(std::clamp(a, 0.f, 1.f) * 255.f));
}

/* @the face and the atlas it lives in, resolved together.

   Asking for them separately was a bug waiting to happen: the font fell back on
   ensure() failing, while a texture picked on texture_id() != 0 could still be
   the named pool's — a face refused by the failed-list leaves the last good
   texture standing. The two would then disagree, and AddText would sample one
   atlas's UVs out of another atlas's texture: glyphs drawn as garbage, or as
   nothing at all. One function, one decision. */
struct picked_font
{
    ImFont* font = nullptr;
    ImTextureID texture = 0;
};

picked_font pick_font(float size)
{
    if (targethud::ensure())
    {
        if (ImFont* font = targethud::get(size))
            return picked_font{ font, targethud::texture_id() };
    }

    /* The named pool failing is not a reason to draw nothing — the arraylist's
       pool is always there, and a HUD in the wrong typeface is a visible,
       fixable problem where an empty HUD looks like a broken module. */
    auto& fallback = arraylist::shared_pool();

    if (!fallback.ensure())
        return {};

    return picked_font{ fallback.get(size), fallback.texture_id() };
}

ImVec2 measure(ImFont* font, float size, const char* text)
{
    return font != nullptr ? font->CalcTextSizeA(size, FLT_MAX, -1.f, text) : ImVec2(0.f, 0.f);
}

/* @the world anchor, projected and smoothed.

   Three outcomes, not two, and the distinction is the whole fix:

     placed      the target projected onto the screen; the panel goes beside it.

     offscreen   there IS a camera frame, the target simply is not in it -- behind
                 you, above you up a ladder, past the edge. The panel does not
                 belong anywhere on screen, so the caller freezes it where it was
                 and fades it out. Clamping it to the edge instead is what made it
                 sit in a corner attached to nobody, and falling back to the fixed
                 anchor is what made it appear to teleport and stop following.

     unavailable no camera frame at all -- the capture has not installed yet. That
                 is a client failure rather than a fact about the target, so the
                 caller uses the fixed anchor and the panel stays visible. */
enum class follow_result { unavailable, offscreen, placed };
/* @Breeze's numbers, not approximations of them.

   Both half-lives, the anchor and the placement are lifted from the reference
   client rather than retuned by feel: 60ms on the world point, 12ms on the
   projected one, anchored at half the entity's height, panel to the left with a
   10px-times-scale gap, clamped to the framebuffer. They are fixed constants
   there — the mode is a single boolean with no smoothing knob — and a knob is
   exactly what would stop this being the same configuration. */
constexpr double k_world_half_life = 0.06;
constexpr double k_screen_half_life = 0.012;

follow_result follow_anchor(int display_w, int display_h, float panel_w, float panel_h,
    float scale, float delta, float& out_x, float& out_y)
{
    const target_snapshot& target = latest();

    hf::render_frame frame{};

    if (!hf::latest_frame(frame))
        return follow_result::unavailable;

    /* Half the entity's height — the middle of the body. A panel level with the
       chest reads as belonging to the player; one over their head reads as a
       nameplate and collides with the real one. */
    const double anchor_x = target.x;
    const double anchor_y = target.y + target.height * 0.5;
    const double anchor_z = target.z;

    /* @stage one: smooth in the WORLD.

       This is the half that was missing before, and it is the whole difference.
       Smoothing only the projected point makes the panel lag behind every time
       the CAMERA moves — turn your view and the entire projection jumps, the
       easing drags the panel across the screen after it, and what you see is a
       panel sliding around rather than one attached to anybody. Smoothing the
       world point removes the target's own 20Hz stepping while leaving camera
       motion instantaneous, which is correct: when you turn, the player moves on
       screen immediately, so the panel must too. */
    if (!g_world_seeded)
    {
        g_world_x = anchor_x;
        g_world_y = anchor_y;
        g_world_z = anchor_z;
        g_world_seeded = true;
    }
    else
    {
        g_world_x = decay(g_world_x, anchor_x, k_world_half_life, delta);
        g_world_y = decay(g_world_y, anchor_y, k_world_half_life, delta);
        g_world_z = decay(g_world_z, anchor_z, k_world_half_life, delta);
    }

    float sx = 0.f, sy = 0.f, depth = 0.f;

    if (!hf::world_to_screen(frame, g_world_x, g_world_y, g_world_z, sx, sy, depth))
        return follow_result::offscreen;

    /* @off the edge is not "against the edge".

       The clamp below is right while the player is ON screen -- it keeps the panel
       whole when they walk near a border. Applied to a player who has left the
       view it is a lie: the panel parks in the corner and follows nothing. So the
       projected point has to actually be inside the viewport before the clamp is
       allowed to have an opinion. */
    if (!std::isfinite(sx) || !std::isfinite(sy))
        return follow_result::offscreen;

    /* Asymmetric on purpose: it takes a clear margin to leave the view and none
       to re-enter it, so a target walking the border settles on one answer
       instead of alternating between them every frame. */
    const float leave = 48.f * scale;
    const float slack = g_follow_onscreen ? leave : 0.f;

    if (sx < -slack || sy < -slack
        || sx > static_cast<float>(display_w) + slack
        || sy > static_cast<float>(display_h) + slack)
    {
        g_follow_onscreen = false;
        return follow_result::offscreen;
    }

    g_follow_onscreen = true;

    /* To the LEFT of the target, vertically centred on it. */
    float wanted_x = sx - panel_w - 10.f * scale;
    float wanted_y = sy - panel_h * 0.5f;

    wanted_x = std::clamp(wanted_x, 0.f, std::max(0.f, static_cast<float>(display_w) - panel_w));
    wanted_y = std::clamp(wanted_y, 0.f, std::max(0.f, static_cast<float>(display_h) - panel_h));

    /* @stage two: a much lighter smooth on the projected point.

       An order of magnitude shorter than the world one. This is not where the
       following happens — it is here to take the last frame of numerical jitter
       out of the projection, and anything heavier would reintroduce the camera
       lag stage one exists to remove. */
    if (!g_follow_seeded || g_follow_resnap)
    {
        g_follow_x = wanted_x;
        g_follow_y = wanted_y;
        g_follow_seeded = true;
        g_follow_resnap = false;
    }
    else
    {
        g_follow_x = static_cast<float>(decay(g_follow_x, wanted_x, k_screen_half_life, delta));
        g_follow_y = static_cast<float>(decay(g_follow_y, wanted_y, k_screen_half_life, delta));
    }

    out_x = g_follow_x;
    out_y = g_follow_y;
    return follow_result::placed;
}

} // namespace

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    g_layout.valid = false;

    if (display_w < 2 || display_h < 2)
        return;

    const float delta = tick_delta();

    /* Slow enough to be a sheen and not a light show. Advanced whether or not
       anything is drawn, so the sweep does not restart from the left every time
       a target appears. */
    g_sweep += delta * 0.32f;

    if (g_sweep >= 1.f)
        g_sweep -= std::floor(g_sweep);

    if (!g_enabled)
    {
        g_shown_name.clear();
        g_drawn = target_snapshot{};
        g_world_seeded = false;
        /* Still ticked while disabled so the fade completes instead of freezing
           at whatever the last enabled frame left, and so re-enabling starts
           from rest rather than resuming a half-finished animation. */
        poll(delta, 0.f, 0.f);
        g_follow_seeded = false;
        g_world_seeded = false;
        g_health_seeded = false;
        /* Back to fully visible, so re-enabling the module does not start the
           panel mid-fade from wherever the last target happened to walk off. */
        g_follow_vis = 1.f;
        return;
    }

    if (g_settings_provider != nullptr)
        g_settings_provider(g_settings);

    targethud::select(static_cast<face_id>(g_settings.font));

    /* The projection matrices come from hitfx's capture, which reads the game's
       own render state over JNI rather than hooking the render method. Asking
       for it is only needed in Follow mode, but the install is rate-limited and
       idempotent, and asking a frame early costs nothing. */
    if (g_settings.follow)
    {
        if (!cap::is_installed())
        {
            static double next_attempt = 0.0;
            const double now = ImGui::GetTime();

            if (now >= next_attempt)
            {
                next_attempt = now + 1.0;
                cap::install();
            }
        }

        cap::set_capture_enabled(true);

        /* @and then actually take the picture.

           This is why Follow did nothing. install() only resolves the reflection;
           capture_now() is what reads MC's projection, view and camera position
           and publishes the frame that world_to_screen consumes. Only the ESP and
           HitFX were calling it — so Follow worked exactly when one of those two
           happened to be enabled, and with both off latest_frame() never had
           anything, follow_anchor returned false every frame, and the panel sat
           silently on its fixed anchor as though the setting were not wired up.

           Taken here rather than borrowed, so the mode does not depend on an
           unrelated module being on. */
        vanta::jvm::JniScope scope;

        if (scope)
            cap::capture_now(scope.get());
    }

    poll(delta, std::max(g_settings.linger, 0.f),
        std::clamp(g_settings.range, 4.f, 64.f));

    /* @the editor gets a stand-in, because there is nothing to fight in it.

       Every other HUD element is always drawn, so the editor always has a bbox
       to hang a drag handle on. This one only exists while someone is under your
       crosshair — and while the editor is open you are looking at a grid, not at
       a player. Without a stand-in the panel is never laid out, query_bbox
       returns zeros, and the editor has nothing to draw a handle around: the
       element is simply absent from the one screen whose whole job is placing
       it.

       Your own head and name, not a fake one: the local account's head is
       already fetched and cached, so the preview is a real panel at its real
       size rather than a rectangle standing in for one. */
    const bool editing = vanta::vgui::bridge::isHudEditorActive();

    if (editing)
    {
        std::string self = ::vanta::profile::name();

        g_drawn.valid = true;
        g_drawn.name = self.empty() ? std::string{ "Target" } : std::move(self);
        g_drawn.max_health = 20.f;
        g_drawn.health = 15.f;
        g_drawn.distance = 3.2f;
        g_drawn.height = 1.8f;

        g_shown_health = g_drawn.health;
        g_health_seeded = true;
        g_shown_name = g_drawn.name;
    }
    else
    {
        g_drawn = latest();

    }

    const target_snapshot& target = g_drawn;

    /* @no name, no panel -- and the glass pass has to agree with the text pass.

       render_text bails on an empty name, but render_background did not, so on a
       world change (disconnect, portal, /hub) the snapshot was wiped while the
       fade was still running and the frosted pane kept being composited: an empty
       blurred card popped up at the anchor and faded out with nothing in it. Only
       visible with Glass on, which is exactly how a disagreement between two
       renderers of the same panel shows up. */
    if (!editing && target.name.empty())
    {
        g_follow_seeded = false;
        g_world_seeded = false;
        g_follow_onscreen = false;
        g_follow_vis = 1.f;
        return;
    }

    const bool have = editing || (target.valid && !target.name.empty());
    const float reveal = editing ? 1.f : presence();

    /* @"Always" keeps the last target on screen; "On Target" lets it go.

       These used to be two separate tests, and the second was a strict subset of
       the first — so it could never be reached and the mode did nothing at all.
       One test, and the mode is what decides it. */
    const bool keep = static_cast<visibility>(g_settings.show) == visibility::always
        && !target.name.empty();

    if (!have && !keep && reveal <= 0.004f)
    {
        g_follow_seeded = false;
        g_world_seeded = false;
        g_health_seeded = false;
        g_follow_onscreen = false;
        /* Restored here too. This return is normally reached with the spring at 0
           -- it decays faster than presence does -- and leaving it there made the
           NEXT target fade in as the product of two ramps instead of one. */
        g_follow_vis = 1.f;
        return;
    }

    /* ── health easing ───────────────────────────────────────────────────── */

    /* Eased BEFORE the metrics, because the readout is formatted from this value
       and the panel is sized around that string. Updating it afterwards would
       measure last frame's number and lay out this frame's — a one-frame width
       lag that shows up as the panel breathing every time the target is hit. */
    /* @a different player is not a health change, it is a different bar.

       Easing across a switch drains the new target's bar from the old one's
       value — look from a full-health enemy to a hurt one and the bar glides
       down as if you had just hit them. The identity has to reset the easing the
       same way it resets the follow position. */
    if (have && !editing && target.name != g_shown_name)
    {
        g_shown_name = target.name;
        g_health_seeded = false;
        g_follow_seeded = false;
        g_world_seeded = false;
    }

    if (have && !editing)
    {
        if (!g_health_seeded)
        {
            g_shown_health = target.health;
            g_health_seeded = true;
        }
        else
        {
            g_shown_health = approach(g_shown_health, target.health, 11.f, delta);
        }
    }

    /* ── metrics ─────────────────────────────────────────────────────────── */

    /* Scaled off the framebuffer height so the HUD is the same physical size on
       1080p and 1440p — the arraylist and the watermark both do this, and a HUD
       that is half the size on a bigger monitor is the bug it prevents. */
    /* Held at full opacity in Always mode. presence() decays once the linger
       runs out — that is what "On Target" wants and precisely what "Always" must
       not do. */
    const float shown_alpha_base = keep ? 1.f : reveal;

    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float scale = dpi * std::clamp(g_settings.scale, 0.5f, 2.5f);

    /* @the base size, before the Scale setting touches it.

       Sized up from the first cut. A Target HUD is read mid-fight, in peripheral
       vision, against whatever the world happens to be behind it — the numbers
       that make a settings row legible at rest are not the numbers that make a
       HUD legible while you are strafing. Scale still moves all of it together;
       this is what 1.0 now means. */
    const float pad = 10.f * scale;
    const float head = 42.f * scale;
    const float gap = 12.f * scale;   /* head to text — "a bit right, not glued" */

    const float name_size = 15.5f * scale;
    const float value_size = 12.f * scale;
    const float bar_h = 7.f * scale;

    const picked_font name_pick = pick_font(name_size);
    const picked_font value_pick = pick_font(value_size);

    if (name_pick.font == nullptr || value_pick.font == nullptr)
        return;

    char readout[24];
    std::snprintf(readout, sizeof(readout), "%.1f", static_cast<double>(g_shown_health));

    /* @two numbers, two places.

       They used to be one string on one line. They answer different questions —
       how much fight is left in them, and how far away they are — and the health
       number belongs against the bar it labels while the distance belongs with
       the name, which is the other thing that identifies rather than measures. */
    char distance_text[16] = {};

    if (g_settings.show_distance && target.distance > 0.01f)
        std::snprintf(distance_text, sizeof(distance_text), "%.0fm", static_cast<double>(target.distance));

    const ImVec2 name_extent = measure(name_pick.font, name_size, target.name.c_str());
    const ImVec2 value_extent = measure(value_pick.font, value_size, readout);

    /* @the distance gets a fixed slot, not its own width.

       Measuring the live string made the panel breathe: this face's digits are
       not tabular -- a '1' is 0.380 em against a '0' at 0.617 -- so every metre
       walked re-measured the row and moved the panel edge, and with it the health
       bar, which is sized from the same column. Reserving the widest reading the
       Range allows means the number can change every frame and nothing moves. */
    const ImVec2 distance_extent = distance_text[0] != '\0'
        ? measure(value_pick.font, value_size, "000m") : ImVec2(0.f, 0.f);

    /* @three stacked rows, not two.

       The readout used to share the name's baseline, which put it at the very top
       of the panel — a long way from the bar it describes. It belongs directly
       above the bar: near enough that the eye reads the two as one gauge, far
       enough that it is not sitting on it. */
    const float name_gap = 5.f * scale;
    const float value_gap = 4.f * scale;

    /* The column is whichever is wider — the name or a bar long enough to be a
       bar. A bar that shrinks to the width of a short username stops being
       readable as a gauge. */
    const float min_bar = 118.f * scale;

    /* The top row now has to hold the name AND the distance without them
       touching, so it, not the bar, is usually what sets the width. */
    const float top_row = name_extent.x
        + (distance_extent.x > 0.f ? 12.f * scale + distance_extent.x : 0.f);

    const float column_w = std::max(min_bar, std::max(top_row, value_extent.x));

    const float w = pad + head + gap + column_w + pad;
    const float column_h = name_extent.y + name_gap + value_extent.y + value_gap + bar_h;
    const float body_h = std::max(head, column_h);
    const float h = body_h + pad * 2.f;

    /* ── placement ───────────────────────────────────────────────────────── */

    float x = 0.f;
    float y = 0.f;

    bool placed = false;

    /* Follow is suspended in the editor. The thing being placed there is the
       fixed anchor, and a panel that flies off to a projected world point while
       you are dragging it is not placeable at all.

       Gated on there being a snapshot to follow rather than on `have`: the lock's
       valid flag drops the instant the linger expires, but the panel is still on
       screen for the whole fade — and permanently in Always mode. The snapshot's
       world position deliberately outlives that flag, so following it keeps the
       panel where it was instead of teleporting it to the fixed anchor at full
       opacity and fading out over there. */
    /* Frozen where it last sat, because the target has left the view. Distinct
       from `placed`: there is a position to draw at, but it is on its way out. */
    bool frozen = false;

    /* @and the snapshot has to still be LIVE.

       The name outlives the lock on purpose, which is what lets the panel keep
       following through its fade. In Always mode it outlives it forever -- so
       following it forever meant chasing a world point that poll() can no longer
       refresh, and once that ghost point fell behind the camera the panel was
       pinned invisible for the rest of the session. presence() is the difference:
       it is still up during the fade and long gone in Always. */
    const bool following = g_settings.follow && !editing && !latest().name.empty()
        && (latest().valid || presence() > 0.004f);

    /* 1 while the placement is honest, 0 while it is not. Not a boolean, so the
       panel dissolves as the player crosses the edge of the screen rather than
       blinking off on one frame. */
    float vis_target = 1.f;

    if (following)
    {
        switch (follow_anchor(display_w, display_h, w, h, scale, delta, x, y))
        {
        case follow_result::placed:
            placed = true;
            break;

        case follow_result::offscreen:
            vis_target = 0.f;

            /* Held at the last good position for the length of the fade. Moving it
               anywhere during those few frames is the teleport this is fixing. */
            if (g_follow_seeded)
            {
                x = g_follow_x;
                y = g_follow_y;
                frozen = true;
            }

            /* The return is a SNAP, not a flight -- by then the panel is invisible,
               so it costs nothing to put it straight onto the player, where easing
               from wherever they left the screen drags it across the whole view.
               Flagged rather than unseeded, because g_follow_x/y are what the hold
               above is still drawing at. */
            g_follow_resnap = true;
            g_world_seeded = false;
            break;

        case follow_result::unavailable:
            /* No camera frame at all. Nothing has been learned about the target,
               so the fixed anchor is the honest answer and the panel stays up. */
            break;
        }
    }

    g_follow_vis = approach(g_follow_vis, vis_target, 16.f, delta);

    if (!placed && !frozen)
    {
        if (!std::isnan(g_anchor_x) && !std::isnan(g_anchor_y))
        {
            x = g_anchor_x;
            y = g_anchor_y;
        }
        else
        {
            /* @middle of the screen, hard right.

               Vertically centred rather than anywhere near the bottom: that half
               of the screen is already spoken for by the hotbar, the health and
               hunger rows and the chat, and a panel that lands on top of any of
               them has to be moved before it can be used. The right edge keeps it
               clear of the watermark, and the vertical centre puts it below the
               arraylist rather than under it — the two share the edge without
               ever overlapping.

               Still only a default; the HUD editor overrides it the moment the
               user drags the panel anywhere. */
            x = static_cast<float>(display_w) - w - 24.f * dpi;
            y = (static_cast<float>(display_h) - h) * 0.5f;
        }

        /* Both seeds, not just the screen one. A stale world point is the worse
           half: it survives a whole linger, so re-hitting the same player after
           they have walked off makes the panel appear at where they used to be
           and fly across to them. */
        g_follow_seeded = false;
        g_world_seeded = false;
        g_follow_onscreen = false;
    }

    /* ── publish ─────────────────────────────────────────────────────────── */

    /* Folded in only now, because g_follow_vis is not known until the placement
       above has run. */
    const float shown_alpha = shown_alpha_base * std::clamp(g_follow_vis, 0.f, 1.f);

    const float radius_base = 6.f * scale * std::clamp(g_settings.rounding, 0.f, 2.f);
    const float radius = std::min(radius_base, std::max(0.f, std::min(w, h) * 0.5f - 1.f));

    /* @one line a second, and only while the module is on.

       Three rounds have now been spent guessing why the panel is or is not on
       screen from a screenshot, which cannot distinguish "never laid out" from
       "laid out somewhere you are not looking" from "laid out and then not
       drawn". This says which. */
    {
        static double next_report = 0.0;
        const double now = ImGui::GetTime();

        if (now >= next_report)
        {
            next_report = now + 1.0;

            VLOG_INFO("targethud",
                "editing={} have={} alpha={} follow={} captured={} vis={} at {}x{} size {}x{} name='{}'",
                editing ? 1 : 0, have ? 1 : 0, shown_alpha,
                g_settings.follow ? 1 : 0, placed ? 1 : 0, g_follow_vis,
                static_cast<int>(x), static_cast<int>(y),
                static_cast<int>(w), static_cast<int>(h), target.name);
        }
    }

    g_layout.valid = true;
    g_layout.x = x;
    g_layout.y = y;
    g_layout.w = w;
    g_layout.h = h;
    g_layout.radius = radius;
    g_layout.alpha = shown_alpha;
    g_layout.scale = scale;

    g_layout.head_size = head;
    g_layout.head_x = x + pad;
    g_layout.head_y = y + (h - head) * 0.5f;

    g_layout.column_x = g_layout.head_x + head + gap;
    g_layout.column_w = column_w;

    g_layout.name_size = name_size;
    g_layout.value_size = value_size;

    g_layout.bar_x = g_layout.column_x;
    g_layout.bar_w = column_w;
    g_layout.bar_h = bar_h;

    /* The column is centred against the head rather than pinned to the padding,
       so a short name does not leave the text floating at the top of a panel
       whose height the head is setting. */
    const float column_top = y + (h - column_h) * 0.5f;

    g_layout.name_y = column_top;
    g_layout.value_y = column_top + name_extent.y + name_gap;
    g_layout.bar_y = g_layout.value_y + value_extent.y + value_gap;

    /* ── the frosted pane ────────────────────────────────────────────────── */

    /* Only when the panel itself is frosted. With Glass off the client is a set
       of flat cards, and one acrylic surface floating among them does not read
       as the same product. */
    if (!vanta::vgui::bridge::isGlassEnabled() || !g_glass.available())
        return;

    /* The composite discards at zero coverage, but only after the framebuffer has
       been captured and blurred nine times. That is the whole cost of the effect,
       paid every frame, for a pane nobody can see. */
    if (shown_alpha <= 0.004f)
        return;

    const float y_bottom = static_cast<float>(display_h) - (y + h);

    std::vector<al::al_rect> rects;
    rects.emplace_back(x, y_bottom, w, h, radius, radius, radius, radius);

    const int bbox_pad = static_cast<int>(std::ceil(scale * 16.0));
    const int bx = static_cast<int>(std::floor(x)) - bbox_pad;
    const int by = static_cast<int>(std::floor(y_bottom)) - bbox_pad;
    const int bw = static_cast<int>(std::ceil(w)) + 2 * bbox_pad;
    const int bh = static_cast<int>(std::ceil(h)) + 2 * bbox_pad;

    /* @black at 45%, not #0e0e0e at 78% -- and that was the whole reason it did
       not look like glass.

       The shader computes mix(blur, tint.rgb, tint.a) * brightness, so the old
       pair let through 0.22 * 0.74 = 16% of the blurred scene and then added a
       constant grey floor of 0.032 on top of it. Sixteen percent of anything is
       not a frosted surface; it is a flat dark card with a rumour of the world
       behind it, which is exactly how it read.

       The rest of the client already had the answer. The arraylist and the
       watermark both use PURE BLACK at 95/255 with brightness 0.65 -- 41% of the
       scene and no floor at all -- and they are the surfaces that read as glass.
       Matching them: 0.55 * 0.72 = 40%, two and a half times more world coming
       through, and no grey floor to flatten the contrast.

       Pure black rather than #0e0e0e for the tint specifically because a tint is
       not a fill: #0e0e0e as a tint adds its own grey to every pixel, which lifts
       the blacks and washes the pane out. The panel keeps its #0e0e0e identity
       where that identity belongs -- the flat card drawn when Glass is off. */
    const al::al_tint tint{ 0.f, 0.f, 0.f, 0.45f };

    g_glass.render(rects,
        display_w, display_h,
        bound_fb,
        bx, by, bw, bh,
        /* Deeper than the arraylist's, and not tied to its setting. That one is a
           strip of text rows over the whole screen edge; this is a small solid
           card, and what shows through a small card has to be genuinely
           unreadable or the panel stops being a surface and becomes a window. */
        /* One more pass than before. Each one widens the kernel, and a wider blur
           is the difference between a frosted surface and a smeared photograph of
           what is behind it. */
        std::max(arraylist::g_settings.blur_passes, 10),
        /*brightness=*/0.72f,
        tint,
        /*edge_softness=*/std::max(1.5f, 1.75f * scale),
        /*shadow_radius=*/10.f * scale,
        /*shadow_strength=*/0.22f,
        /*shadow_gamma=*/1.35f,
        /*opacity=*/shown_alpha);
}

void render_text(int display_w, int display_h)
{
    if (!g_enabled || !g_layout.valid)
        return;

    const target_snapshot& target = g_drawn;

    if (target.name.empty())
        return;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    if (draw_list == nullptr)
        return;

    const float alpha = std::clamp(g_layout.alpha, 0.f, 1.f);

    if (alpha <= 0.004f)
        return;

    /* gl_hook seeds io.DisplaySize with the 1120x700 GUI canvas, so every draw
       list opens the frame clipped to that. HUD geometry is in real framebuffer
       pixels and on any display wider than 1120 the right-hand half would be
       scissored away — hence a NON-intersecting clip rect covering the whole
       framebuffer. The arraylist documents the same trap. */
    draw_list->PushClipRect(ImVec2(0.f, 0.f),
        ImVec2(static_cast<float>(display_w), static_cast<float>(display_h)), false);

    /* ── background ──────────────────────────────────────────────────────── */

    /* With Glass on the pane was already composited by the shader pass, and
       painting the flat card over it would bury the blur under the very colour
       it was meant to replace. */
    if (!vanta::vgui::bridge::isGlassEnabled() || !g_glass.available())
    {
        /* Fully opaque, not 94%. #0e0e0e is a colour, and letting six percent of
           the world through it makes it a slightly different colour depending on
           what you are standing in front of — which is what read as the panel
           being see-through. Only the fade scales it. */
        draw_list->AddRectFilled(
            ImVec2(g_layout.x, g_layout.y),
            ImVec2(g_layout.x + g_layout.w, g_layout.y + g_layout.h),
            rgba(14.f / 255.f, 14.f / 255.f, 14.f / 255.f, alpha),
            g_layout.radius);
    }

    /* ── the edge ────────────────────────────────────────────────────────── */

    /* @a hairline, and NOT a sheen.

       The panel had no edge at all: the flat card ended, and the world began. A
       real pane has a lit rim where the light catches its bevel, and that rim is
       most of what makes a surface read as glass rather than as a hole cut in the
       screen -- more than the blur behind it does. Drawn for the flat card too,
       because the same absence made that one look unfinished.

       There was a highlight above it as well, a white wash over the top 42% of
       the panel meant to read as light falling across the surface. It did not: a
       filled rect has a straight bottom edge, and four percent of white over a
       near-black card is more than enough to see one. What it actually drew was a
       hard horizontal seam across the middle of the panel, which reads as the
       panel being cut in half rather than as anything lit.

       There is no fixing it by softening the number -- the edge is a property of
       the shape, not of the alpha -- and the honest gradient that would replace
       it, AddRectFilledMultiColor, has no rounded corners, so it trades a seam in
       the middle for two square nubs poking out of the top ones. The rim alone
       does the job it was hired for. */
    draw_list->AddRect(
        ImVec2(g_layout.x, g_layout.y),
        ImVec2(g_layout.x + g_layout.w, g_layout.y + g_layout.h),
        rgba(1.f, 1.f, 1.f, 0.10f * alpha),
        g_layout.radius, 0, std::max(1.f, g_layout.scale * 0.9f));

    /* ── head ────────────────────────────────────────────────────────────── */

    const ImTextureID head = heads::texture_for(target.name);

    if (head != 0)
    {
        /* The head is drawn under its OWN texture, so it must not sit inside the
           font atlas push below — and neither may the bar, for the mirror-image
           reason: a filled rect samples the shared "white pixel" UV, which
           indexes ImGui's default atlas. Under a font-pool push that UV lands on
           an unrelated texel and the shape comes out invisible. The watermark's
           separator line was lost to exactly this. */
        draw_list->AddImageRounded(head,
            ImVec2(g_layout.head_x, g_layout.head_y),
            ImVec2(g_layout.head_x + g_layout.head_size, g_layout.head_y + g_layout.head_size),
            ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
            rgba(1.f, 1.f, 1.f, alpha),
            g_layout.head_size * 0.14f);
    }
    else
    {
        /* A placeholder while the fetch is in flight, so the layout does not
           reflow the moment the head lands. */
        draw_list->AddRectFilled(
            ImVec2(g_layout.head_x, g_layout.head_y),
            ImVec2(g_layout.head_x + g_layout.head_size, g_layout.head_y + g_layout.head_size),
            rgba(0.16f, 0.16f, 0.18f, alpha),
            g_layout.head_size * 0.14f);
    }

    /* ── health bar ──────────────────────────────────────────────────────── */

    float accent_r = 0.61f, accent_g = 0.29f, accent_b = 0.86f, accent_a = 1.f;
    vanta::vgui::bridge::clientAccent(accent_r, accent_g, accent_b, accent_a);

    const float ratio = target.max_health > 0.f
        ? std::clamp(g_shown_health / target.max_health, 0.f, 1.f)
        : 0.f;

    const float bar_radius = g_layout.bar_h * 0.5f;

    draw_list->AddRectFilled(
        ImVec2(g_layout.bar_x, g_layout.bar_y),
        ImVec2(g_layout.bar_x + g_layout.bar_w, g_layout.bar_y + g_layout.bar_h),
        rgba(1.f, 1.f, 1.f, 0.09f * alpha),
        bar_radius);

    if (ratio > 0.001f)
    {
        /* Never narrower than its own diameter. A rounded rect thinner than its
           corner radius degenerates — the last sliver of health would vanish
           before the health did, which is the one part of the bar that has to be
           right. */
        const float fill_w = std::max(g_layout.bar_h, g_layout.bar_w * ratio);

        draw_list->AddRectFilled(
            ImVec2(g_layout.bar_x, g_layout.bar_y),
            ImVec2(g_layout.bar_x + fill_w, g_layout.bar_y + g_layout.bar_h),
            rgba(accent_r, accent_g, accent_b, accent_a * alpha),
            bar_radius);
    }

    /* ── text ────────────────────────────────────────────────────────────── */

    const picked_font name_pick = pick_font(g_layout.name_size);
    const picked_font value_pick = pick_font(g_layout.value_size);

    /* Both sizes come out of the same pool — the ladder is one atlas — so one
       texture covers both. Bailing when they somehow differ is cheaper than
       reasoning about which of the two to push. */
    if (name_pick.font == nullptr || value_pick.font == nullptr
        || name_pick.texture != value_pick.texture)
    {
        draw_list->PopClipRect();
        return;
    }

    char readout[24];
    std::snprintf(readout, sizeof(readout), "%.1f", static_cast<double>(g_shown_health));

    char distance_text[16] = {};

    if (g_settings.show_distance && target.distance > 0.01f)
        std::snprintf(distance_text, sizeof(distance_text), "%.0fm", static_cast<double>(target.distance));

    const ImVec2 name_extent = measure(name_pick.font, g_layout.name_size, target.name.c_str());
    const ImVec2 value_extent = measure(value_pick.font, g_layout.value_size, readout);

    draw_list->PushTextureID(name_pick.texture);

    /* @the name is swept across the accent, left to right.

       Drawn a glyph at a time, because a draw list has no gradient for text: a
       colour is per-call, so a gradient means one call per letter with the colour
       stepped along the way. The advance comes from the font rather than from the
       measured width divided by the character count — proportional faces do not
       have a character width, and stepping by an average one makes the sweep
       drift out of phase with the letters it is colouring.

       Two stops of the SAME accent, deep into light, not accent into white: the
       point is a name that reads as part of the client's colour, and a gradient
       that ends in white ends by not being the accent at all. */
    {
        const float total = std::max(1.f, name_extent.x);
        const float shade = g_layout.name_size / std::max(1.f, name_pick.font->FontSize);

        float cursor = g_layout.column_x;

        for (std::size_t index = 0; index < target.name.size(); ++index)
        {
            const char glyph[2] = { target.name[index], '\0' };

            /* @a travelling wave, not a fixed ramp.

               A ramp from one end of the word to the other is a gradient that
               sits still. Subtracting the phase slides the whole pattern to the
               right, and the cosine is what lets it wrap: at u - phase = 0 and
               = 1 the value is identical, so the band leaving the right edge is
               the same band arriving at the left and there is no seam where it
               restarts. A sawtooth ramp would jump. */
            const float u = std::clamp((cursor - g_layout.column_x) / total, 0.f, 1.f);

            constexpr float k_two_pi = 6.28318530717958647692f;

            const float t = 0.5f - 0.5f * std::cos((u - g_sweep) * k_two_pi);

            const float gr = accent_r + (std::min(accent_r + 0.42f, 1.f) - accent_r) * t;
            const float gg = accent_g + (std::min(accent_g + 0.42f, 1.f) - accent_g) * t;
            const float gb = accent_b + (std::min(accent_b + 0.42f, 1.f) - accent_b) * t;

            draw_list->AddText(name_pick.font, g_layout.name_size,
                ImVec2(cursor, g_layout.name_y),
                rgba(gr, gg, gb, 0.98f * alpha), glyph);

            cursor += name_pick.font->GetCharAdvance(
                static_cast<ImWchar>(static_cast<unsigned char>(target.name[index]))) * shade;
        }
    }

    /* Grey and right-aligned, on its own row directly above the bar. Subdued on
       purpose: the number is a detail you check, the bar is the thing you read at
       a glance, and a bright readout competes with it. */
    draw_list->AddText(value_pick.font, g_layout.value_size,
        ImVec2(g_layout.column_x + g_layout.column_w - value_extent.x, g_layout.value_y),
        rgba(0.62f, 0.62f, 0.66f, 0.95f * alpha),
        readout);

    /* Top right, on the name's line and dimmer than it. Distance is context, not
       a reading you act on — it should be there when you look for it and out of
       the way when you are not. */
    if (distance_text[0] != '\0')
    {
        const ImVec2 distance_extent = measure(value_pick.font, g_layout.value_size, distance_text);

        draw_list->AddText(value_pick.font, g_layout.value_size,
            ImVec2(g_layout.column_x + g_layout.column_w - distance_extent.x,
                   g_layout.name_y + (g_layout.name_size - g_layout.value_size) * 0.55f),
            rgba(0.55f, 0.55f, 0.60f, 0.9f * alpha),
            distance_text);
    }

    draw_list->PopTextureID();

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
    /* @the drag is accepted in Follow mode too, and refusing it was the bug.

       The reasoning for refusing was that in Follow the position belongs to the
       target, so a drag would be undone on the next frame. That is not what
       happens. Follow is SUSPENDED in the editor -- render_background gates it on
       !editing, precisely so the panel holds still while you place it -- so the
       thing under your cursor in there is the fixed anchor, and the anchor is
       exactly what a drag is meant to edit.

       It is not a dead setting in Follow mode either: it is the fallback the
       panel uses on every frame Follow cannot produce a position, which is any
       frame the target is off screen, behind you, or not yet projected. So the
       old guard made the editor silently ignore the drag -- the handle highlighted,
       the panel did not move -- for anyone who had Follow on, which is the exact
       symptom reported. */
    g_anchor_x = x;
    g_anchor_y = y;
}

} // namespace vanta::modules::targethud
