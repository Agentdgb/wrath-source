#include "spotify.h"
#include "ember_motion.h"
#include "../arraylist/al_glass.h"
#include "../arraylist/font_pool.h"
#include "../arraylist/arraylist.h"
#include "vgui/bridge.h"

#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <gl/GL.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace dt::modules::spotify {

namespace { std::atomic<bool> g_chat_open{false}; }
void set_chat_open(bool open) { g_chat_open.store(open, std::memory_order_relaxed); }
bool chat_open() { return g_chat_open.load(std::memory_order_relaxed); }
bool       g_enabled = false;
settings   g_settings{};
std::mutex g_settings_mx;
std::function<void(settings&)> g_settings_provider{};
writers    g_writers{};

settings get_settings_snapshot()
{
    std::lock_guard<std::mutex> lk(g_settings_mx);
    return g_settings;
}

namespace {

al::al_glass g_glass{};

/* Second font pool for the Spotify HUD's artist label + times — Proxima
   Regular so the label reads distinctly from the bold title. Title still
   goes through arraylist::shared_pool (proximabd). */
arraylist::font_pool& regular_pool()
{
    static arraylist::font_pool p{"fonts/proxima.ttf"};
    return p;
}

struct cached_layout {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    bool  valid = false;
};
cached_layout g_layout{};

float g_override_x = std::numeric_limits<float>::quiet_NaN();
float g_override_y = std::numeric_limits<float>::quiet_NaN();

std::mutex g_track_mx;
long long g_play_pending_until = 0;
long long g_shuffle_pending_until = 0;
long long g_seek_pending_until = 0;
// No demo song: the HUD stays empty until the media worker reports playback.
track_info g_track{};

ImU32 pack_rgba(float r, float g, float b, float a)
{
    auto b8 = [](float v) {
        v = std::clamp(v, 0.f, 1.f);
        return static_cast<int>(v * 255.f + 0.5f);
    };
    return IM_COL32(b8(r), b8(g), b8(b), b8(a));
}

double nowSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void draw_ember(ImDrawList* dl,ImTextureID texture,float x,float y,float w,float h,float radius,float scrimStrength)
{
    static const double start=nowSeconds();
    const double seconds=nowSeconds()-start;
    const float r=std::clamp(radius,0.f,std::min(w,h)*.5f);
    constexpr int columns=24, capSteps=16, middleSteps=20;
    constexpr int rows=capSteps*2+middleSteps;
    struct Vertex { ImVec2 pos,uv; };
    std::array<Vertex,(columns+1)*(rows+1)> vertices{};
    // Sample the rounded silhouette densely at the corners. Shared mesh
    // vertices keep the flowing texture continuous across every cell.
    for(int row=0;row<=rows;++row) {
        const float py=row<=capSteps ? r*(1.f-std::cos(1.570796327f*row/capSteps)) : row<=capSteps+middleSteps ?
            r+(h-2*r)*(row-capSteps)/middleSteps : h-r+r*std::sin(1.570796327f*(row-capSteps-middleSteps)/capSteps);
        const float dy=py<r?r-py:py>h-r?py-(h-r):0.f;
        const float inset=dy>0?r-std::sqrt(std::max(0.f,r*r-dy*dy)):0.f;
        for(int col=0;col<=columns;++col) {
            const float px=inset+(w-2*inset)*col/columns;
            const auto uv=ember_uv(px/w,py/h,seconds);
            vertices[row*(columns+1)+col]={{x+px,y+py},{uv[0],uv[1]}};
        }
    }
    dl->PushTextureID(texture);
    auto shade=[&](float py) {
        const float t=std::clamp((py-y)/h,0.f,1.f);
        const int value=static_cast<int>(255.f*.922f*(1.f-(.10f+.30f*t*t*(3.f-2.f*t))*scrimStrength));
        return IM_COL32(value,value,value,255);
    };
    for(int row=0;row<rows;++row) for(int col=0;col<columns;++col) {
        const auto& a=vertices[row*(columns+1)+col];
        const auto& b=vertices[row*(columns+1)+col+1];
        const auto& c=vertices[(row+1)*(columns+1)+col+1];
        const auto& d=vertices[(row+1)*(columns+1)+col];
        dl->AddImageQuad(texture,a.pos,b.pos,c.pos,d.pos,a.uv,b.uv,c.uv,d.uv,IM_COL32_WHITE);
        const int first=dl->VtxBuffer.Size-4;
        for(int i=first;i<dl->VtxBuffer.Size;++i) dl->VtxBuffer[i].col=shade(dl->VtxBuffer[i].pos.y);
    }
    // Textured quads have no automatic ImGui edge antialiasing. Add a
    // one-pixel transparent fringe around the silhouette, never between cells.
    std::vector<Vertex> boundary;
    boundary.reserve(2*(columns+rows));
    for(int col=0;col<=columns;++col) boundary.push_back(vertices[col]);
    for(int row=1;row<=rows;++row) boundary.push_back(vertices[row*(columns+1)+columns]);
    for(int col=columns-1;col>=0;--col) boundary.push_back(vertices[rows*(columns+1)+col]);
    for(int row=rows-1;row>0;--row) boundary.push_back(vertices[row*(columns+1)]);
    auto outside=[&](const Vertex& point) {
        const float nx=point.pos.x-std::clamp(point.pos.x,x+r,x+w-r);
        const float ny=point.pos.y-std::clamp(point.pos.y,y+r,y+h-r);
        const float length=std::max(.001f,std::sqrt(nx*nx+ny*ny));
        return ImVec2(point.pos.x+nx/length,point.pos.y+ny/length);
    };
    for(size_t i=0;i<boundary.size();++i) {
        const auto& a=boundary[i];
        const auto& b=boundary[(i+1)%boundary.size()];
        dl->PrimReserve(6,4);
        const unsigned int base=dl->_VtxCurrentIdx;
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base+1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base+2));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base+2));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base+3));
        dl->PrimWriteVtx(a.pos,a.uv,shade(a.pos.y));
        dl->PrimWriteVtx(b.pos,b.uv,shade(b.pos.y));
        dl->PrimWriteVtx(outside(b),b.uv,shade(b.pos.y)&~IM_COL32_A_MASK);
        dl->PrimWriteVtx(outside(a),a.uv,shade(a.pos.y)&~IM_COL32_A_MASK);
    }
    dl->PopTextureID();
}

long long nowNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void format_time_ms(long long ms, char* out, size_t cap)
{
    if (ms < 0) ms = 0;
    const long long total_s = ms / 1000LL;
    const long long m = total_s / 60LL;
    const long long s = total_s % 60LL;
    std::snprintf(out, cap, "%lld:%02lld", m, s);
}

void format_time_ms_neg(long long ms, char* out, size_t cap)
{
    if (ms < 0) ms = 0;
    const long long total_s = ms / 1000LL;
    const long long m = total_s / 60LL;
    const long long s = total_s % 60LL;
    std::snprintf(out, cap, "-%lld:%02lld", m, s);
}

/* Spotify glyph — filled green disc plus three concentric black arcs whose
   virtual centre sits well BELOW the circle. All three share that same
   virtual origin, so the shortest arc lives near the bottom of the disc
   and the widest sweeps across the top — matching the geometry of the
   official Spotify sound-wave mark.

   Using PathArcTo (not bezier quadratics) keeps the curvature exactly
   circular so the arcs read as concentric rings rather than three
   unrelated parabolas. */
void draw_spotify_glyph(ImDrawList* dl, ImVec2 c, float R)
{
    const ImU32 green = IM_COL32(30, 215, 96, 255);
    const ImU32 black = IM_COL32(0, 0, 0, 255);

    dl->AddCircleFilled(c, R, green, 40);

    /* Virtual centre lies below the disc; the arcs are the top of that
       shared circle, sampled at three different radii. */
    const ImVec2 vc(c.x, c.y + R * 1.15f);
    constexpr float PI = 3.14159265358979f;

    struct wave { float radius; float half_span; float thick; };
    const wave arcs[3] = {
        { R * 1.32f, 0.36f, std::max(1.6f, R * 0.16f) }, // top, widest
        { R * 1.05f, 0.44f, std::max(1.5f, R * 0.15f) }, // middle
        { R * 0.80f, 0.52f, std::max(1.3f, R * 0.13f) }, // bottom, tightest
    };
    for (const auto& a : arcs) {
        const float a_min = 1.5f * PI - a.half_span;
        const float a_max = 1.5f * PI + a.half_span;
        dl->PathClear();
        dl->PathArcTo(vc, a.radius, a_min, a_max, 28);
        dl->PathStroke(black, ImDrawFlags_None, a.thick);
    }
}

}

/* ── transport queue ─────────────────────────────────────────────────────── */

namespace {

struct pending_cmd { cmd c; long long arg_ms; long long queued_ns; };

std::mutex               g_cmd_mx;
std::vector<pending_cmd> g_cmds;

/* @which view the card is in.

   Render-thread only -- both are written from the control row and read by the
   layout pass a frame later, and nothing else touches them. */
bool g_expanded    = false;
bool g_lyrics_open = false;

/* @0 closed, 1 open, and every value between is a frame of the transition.

   Driven in the layout pass because that is what decides the card's height:
   animating only the contents would slide words around inside a box that had
   already snapped to its new size. */
float g_lyrics_anim = 0.f;

/* @the cover view: the art IS the card.

   Eased for the same reason the lyrics panel is -- the card changes shape, and
   a shape that changes between two frames reads as a glitch rather than as a
   transition. */
bool  g_cover      = false;
float g_cover_anim = 0.f;

/* @the wide view: artwork left, words right.

   A third shape rather than a variant of the other two, because it is the only
   one that is landscape -- everything in it is laid out against the width
   rather than stacked down the card. */
bool  g_wide      = false;
float g_wide_anim = 0.f;

/* 1 while the current track has lyrics to show, eased so the wide card grows
   into its second column instead of snapping open when they land. */
float g_words_anim = 0.f;

/* @how much of the control row is showing, decided in the layout pass.

   It drives the card's HEIGHT as well as the buttons' opacity: with the panel
   closed nothing can be clicked, so the row is not drawn -- and a card that
   keeps the row's height anyway is just a strip of empty space along its
   bottom edge. Both halves have to agree, so it lives here rather than as a
   static inside the draw. */
float g_ctl_reveal = 0.f;

/* @exactly one shape at a time.

   The three views were three independent booleans, so entering one while
   another was still easing left both animations running and the card was laid
   out from two blends at once -- which is the "it bugs out when I switch" that
   nothing in a single view could explain. Every entry point goes through here
   and the others are turned off in the same statement, so the invariant cannot
   be broken by adding a fourth caller later. */
enum class view : int { player, wide, cover };

void set_view(view v)
{
    g_wide  = (v == view::wide);
    g_cover = (v == view::cover);

    /* @only the COVER shape cannot hold the words.

       This used to clear the flag for every shape but the player, on the
       reasoning that the stacked panel is the player's. But the wide shape
       exists precisely to put the words beside the artwork, so clearing it on
       the way in left the toggle reading "off" while the column drew anyway --
       the reported bug. The cover view is full-bleed artwork and genuinely has
       nowhere to put them. */
    if (v == view::cover)
        g_lyrics_open = false;
}

/* How far the expand affordance on the artwork has faded in. */
float g_art_hint   = 0.f;

/* The smooth position the progress bar extrapolates, republished so the lyrics
   panel highlights off the same clock instead of SMTC's twice-a-second poll. */
long long g_eff_pos_ms = 0;

/* @the worker sleeps between polls; a click must not wait for it to wake.

   Without this a press waited out the rest of the half-second sleep before the
   command was even looked at, which is the lag the button felt like it had. */
std::condition_variable  g_cmd_cv;

/* Small on purpose. Every entry is a click the user made; if they are stacking
   up it means the worker is not draining, and replaying eight-second-old
   clicks at whatever is playing later is worse than losing them. */
constexpr std::size_t k_cmd_cap = 8;

long long now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

void push_command(cmd c, long long arg_ms)
{
    // Reflect seek and shuffle locally on the input frame.
    {
        std::lock_guard<std::mutex> track_lock(g_track_mx);
        if (c == cmd::shuffle) {
            g_track.shuffled = arg_ms != 0;
            g_shuffle_pending_until = nowNanos() + 400'000'000LL;
        }
        if (c == cmd::seek) {
            g_seek_pending_until = nowNanos() + 400'000'000LL;
            g_track.position_ms = arg_ms;
            g_track.snapshot_ns = nowNanos();
        }
    }
    std::lock_guard<std::mutex> lk(g_cmd_mx);

    if (g_cmds.size() >= k_cmd_cap)
        g_cmds.erase(g_cmds.begin());

    g_cmds.push_back({ c, arg_ms, now_ns() });

    g_cmd_cv.notify_all();
}

void wait_for_command(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(g_cmd_mx);
    g_cmd_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [] { return !g_cmds.empty(); });
}

/* @the icon flips before the player has answered.

   A transport call is a round trip through SMTC and back through the next
   poll, so honouring only the real state left the play glyph sitting on its old
   shape for a beat after the press -- which reads as the click not having
   landed. The next poll overwrites this with the truth either way. */
void assume_playing(bool playing)
{
    std::lock_guard<std::mutex> lk(g_track_mx);
    g_track.playing = playing;
    g_play_pending_until = nowNanos() + 400'000'000LL;
}

bool pop_command(cmd& out, long long& arg_ms)
{
    std::lock_guard<std::mutex> lk(g_cmd_mx);

    while (!g_cmds.empty()) {
        const pending_cmd p = g_cmds.front();
        g_cmds.erase(g_cmds.begin());

        /* Stale: the worker was stuck, or Spotify was closed when it was
           clicked. Acting on it now would be acting on a different song. */
        if (now_ns() - p.queued_ns > 2'000'000'000LL)
            continue;

        out = p.c;
        arg_ms = p.arg_ms;
        return true;
    }

    return false;
}

namespace {
std::mutex                        g_lyrics_mx;
std::shared_ptr<const lyrics_doc> g_lyrics;
std::string g_lyrics_key;
}

std::shared_ptr<const lyrics_doc> get_lyrics()
{
    std::lock_guard<std::mutex> lk(g_lyrics_mx);
    return g_lyrics;
}

void begin_lyrics(const std::string& key)
{
    std::lock_guard<std::mutex> lk(g_lyrics_mx);
    g_lyrics_key = key;
    g_lyrics.reset();
}

void set_lyrics(std::shared_ptr<const lyrics_doc> doc)
{
    std::lock_guard<std::mutex> lk(g_lyrics_mx);
    if (doc && doc->key != g_lyrics_key) return;
    g_lyrics = std::move(doc);
}

void set_track(const track_info& t)
{
    std::lock_guard<std::mutex> lk(g_track_mx);
    track_info next = t;
    const long long now = nowNanos();
    if (t.title == g_track.title && t.artist == g_track.artist) {
        // Ignore stale polls briefly while the player applies the click.
        // Rejected operations return to the reported state after this bounded grace period.
        if (now < g_play_pending_until) next.playing = g_track.playing;
        if (now < g_shuffle_pending_until) next.shuffled = g_track.shuffled;
        if (now < g_seek_pending_until) {
            next.position_ms = g_track.position_ms;
            next.snapshot_ns = g_track.snapshot_ns;
        }
    } else {
        g_play_pending_until = g_shuffle_pending_until = g_seek_pending_until = 0;
    }
    g_track = std::move(next);
}

void clear_track()
{
    std::lock_guard<std::mutex> lk(g_track_mx);
    g_track.title.clear();
    g_track.artist.clear();
    g_track.position_ms = 0;
    g_track.duration_ms = 0;
    g_track.playing = false;
    g_track.thumb_tex_id = 0;
    g_track.blurred_bg_tex_id = 0;
}

track_info get_track()
{
    std::lock_guard<std::mutex> lk(g_track_mx);
    return g_track;
}

namespace {
struct pending_upload {
    std::vector<std::uint8_t> px;
    int w = 0, h = 0;
    std::atomic<unsigned int>* out = nullptr;
    std::function<bool()> still_current;
};
std::mutex                  g_pending_mx;
std::vector<pending_upload> g_pending;

void drain_pending_uploads()
{
    std::vector<pending_upload> local;
    {
        std::lock_guard<std::mutex> lk(g_pending_mx);
        local.swap(g_pending);
    }
    for (auto& p : local) {
        if (p.w <= 0 || p.h <= 0 || p.px.empty() || (p.still_current && !p.still_current())) continue;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) continue;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.w, p.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, p.px.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        if (p.still_current && !p.still_current()) { glDeleteTextures(1, &tex); continue; }
        if (p.out) p.out->store(static_cast<unsigned int>(tex), std::memory_order_release);
    }
}
}

void queue_texture_upload(std::vector<std::uint8_t> px, int w, int h,
                          std::atomic<unsigned int>* out_tex, std::function<bool()> still_current)
{
    if (px.empty() || w <= 0 || h <= 0 || !out_tex) return;
    std::lock_guard<std::mutex> lk(g_pending_mx);
    pending_upload p;
    p.px = std::move(px);
    p.w = w; p.h = h;
    p.out = out_tex;
    p.still_current = std::move(still_current);
    g_pending.push_back(std::move(p));
}

void render_background(int display_w, int display_h, unsigned int bound_fb)
{
    g_layout.valid = false;

    drain_pending_uploads();

    if (!g_enabled || display_w < 2 || display_h < 2) return;
    if (g_settings_provider) {
        std::lock_guard<std::mutex> lk(g_settings_mx);
        g_settings_provider(g_settings);
    }

    const auto playback = get_track();
    // Pausing retains the song, artwork, current view and resume control.
    const bool has_track = !playback.title.empty();
    if (!has_track) {
        g_wide = g_cover = g_lyrics_open = false;
        g_wide_anim = g_cover_anim = g_lyrics_anim = g_words_anim = 0.f;
        g_ctl_reveal = g_art_hint = 0.f;
    }

    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float s = dpi * g_settings.scale;

    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        const float want = g_wide ? 1.f : 0.f;
        g_wide_anim += (want - g_wide_anim) * std::clamp(11.f * dt, 0.f, 1.f);
        if (g_wide_anim < 0.0015f) g_wide_anim = 0.f;
        if (g_wide_anim > 0.9985f) g_wide_anim = 1.f;
    }

    const float wa = g_wide_anim;

    // Lyrics move the content within the wide card, without resizing it.
    {
        const auto doc = get_lyrics();
        const bool no_lyrics = doc && doc->empty();
        if (no_lyrics) {
            g_lyrics_open = false;
            g_words_anim = 0.f;
            g_lyrics_anim = 0.f;
        }

        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        /* Availability AND consent. Keyed on have_words alone, the card widened
           and the column appeared for any track that happened to have a sheet,
           whatever the Aa button said. */
        const float want = g_lyrics_open ? 1.f : 0.f;
        g_words_anim += (want - g_words_anim) * std::clamp(9.f * dt, 0.f, 1.f);
    }

    const float wide_extra = 560.f - 300.f;
    // Wide mode keeps its horizontal silhouette when lyrics are hidden.
    const float w = 300.f * s + wide_extra * s * wa;

    /* @taller than it was, to hold a real control row.

       The transport used to be painted over the album art, which is where it
       fit rather than where it belongs: it hid the cover, it only appeared on
       hover, and the two buttons that are not transport -- lyrics and expand --
       had nowhere to live at all. The card now has a row of its own under the
       progress bar, laid out the way a player is: the transport centred, the
       expand control at the left end, the lyrics toggle at the right. */
    /* The player is a fixed block; the lyrics panel is added UNDER it. Both
       halves measure from player_h, so opening the panel cannot drag the
       controls away from the thing they control. */
    /* @the row's height is only spent when the row exists. */
    {
        float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;
        const bool can_press = has_track && (dt::vgui::bridge::panelRect(px, py, pw, ph) || chat_open())
                            && !dt::vgui::bridge::isHudEditorActive();

        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        const float want = can_press ? 1.f : 0.f;
        g_ctl_reveal += (want - g_ctl_reveal) * std::clamp(13.f * dt, 0.f, 1.f);
        if (g_ctl_reveal < 0.0015f) g_ctl_reveal = 0.f;
        if (g_ctl_reveal > 0.9985f) g_ctl_reveal = 1.f;
    }

    const float player_h = (96.f + 36.f * g_ctl_reveal) * s;

    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        const float want = g_lyrics_open ? 1.f : 0.f;
        g_lyrics_anim += (want - g_lyrics_anim) * std::clamp(13.f * dt, 0.f, 1.f);
        if (g_lyrics_anim < 0.0015f) g_lyrics_anim = 0.f;
        if (g_lyrics_anim > 0.9985f) g_lyrics_anim = 1.f;
    }

    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        const float want = g_cover ? 1.f : 0.f;
        g_cover_anim += (want - g_cover_anim) * std::clamp(11.f * dt, 0.f, 1.f);
        if (g_cover_anim < 0.0015f) g_cover_anim = 0.f;
        if (g_cover_anim > 0.9985f) g_cover_anim = 1.f;
    }

    /* Cover mode is a tall card the artwork fills; the player collapses onto a
       band across its bottom. The two heights are blended so the card grows
       into it instead of swapping. */
    const float cover_h = 300.f * s;
    const float stacked_h = (player_h + g_lyrics_anim * 220.f * s) * (1.f - g_cover_anim)
                          + cover_h * g_cover_anim;

    /* @counted, not guessed.

       art 18..218, title 230, artist 249, progress 276, transport 302, bottom
       margin 18 -> 320. The previous 344 was arrived at by making room and left
       a band of nothing under the controls. */
    const float h = stacked_h * (1.f - wa) + 320.f * s * wa;

    float x, y;
    if (!std::isnan(g_override_x) && !std::isnan(g_override_y)) {
        x = g_override_x; y = g_override_y;
    } else {
        x = 12.f;
        y = static_cast<float>(display_h) - h - 12.f;
    }

    g_layout.x = x; g_layout.y = y; g_layout.w = w; g_layout.h = h;
    g_layout.valid = true;

    /* Glass card — same dual-Kawase pipeline the arraylist uses so the HUD
       inherits the consistent frosted look. The album-art texture is drawn
       on top of this in render_text as a translucent layer so the glass
       edge softness is preserved. */
    if (g_glass.available()) {
        const float y_bottom = static_cast<float>(display_h) - (y + h);
        std::vector<al::al_rect> rects;
        const float radius_base = 6.f * g_settings.scale * g_settings.rounding;
        const float cap = std::max(0.f, std::min(w, h) * 0.5f - 1.f);
        const float rad = std::min(radius_base, cap);
        rects.emplace_back(x, y_bottom, w, h, rad, rad, rad, rad);

        /* @the surround the kernel actually reaches for, not a round number.

           al_glass clamps its up-pass loop to eight (al_glass.cpp:102), so
           asking for ten runs as eight -- the count is clamped here too, so
           the number this pad is computed from is the number that executes.
           Pass i samples at 1.5 + i HALF-RES texels, and a half-res texel is
           always exactly two framebuffer pixels (half_w = bbox_w/2, uTexel =
           1/half_w), so the support is 2 * sum(1.5..8.5) = 80 px, plus ~2 from
           the downsample tap. Fixed in framebuffer pixels -- independent of the
           display and of the Scale slider, which is why the old scale-relative
           18 was wrong twice over.

           Eighteen pixels of surround for an 82-pixel kernel is what
           panel_glass.cc:129 warns about: everything past the box clamps to its
           border row, that one row lands under a third of the kernel, and the
           pane loses its gradient and takes a flat rim along the edge -- most
           visibly along the bottom, where the card sits twelve pixels off the
           screen. Enlarging the box does NOT change the blur's strength; the
           offsets are in half-res texels either way. It only gives the kernel
           real pixels to read. */
        const int blur_passes = std::clamp(
            std::max(arraylist::g_settings.blur_passes, 10), 1, 8);
        const int pad = blur_passes * (blur_passes + 2) + 4;
        const int bx  = static_cast<int>(std::floor(x)) - pad;
        const int by  = static_cast<int>(std::floor(y_bottom)) - pad;
        const int bw  = static_cast<int>(std::ceil(w)) + 2 * pad;
        const int bh  = static_cast<int>(std::ceil(h)) + 2 * pad;

        /* Even tighter shadow — arraylist values still traced a visible
           dark ring right at the rounded edges (the "outline" the user
           kept flagging). Halving shadow_strength and gamma lifts that
           ring out. Blur pass count bumped so the frosted panel behind
           the album art feels genuinely diffuse instead of a mild tint. */
        /* @the house recipe, at last.

           This pane was mixed by hand -- black at 31%, brightness 0.70 -- and
           came out lighter and flatter than every other frosted surface in the
           client. The shader computes mix(blur, tint.rgb, tint.a) * brightness,
           so the numbers below let through 0.55 * 0.72 = 40% of the blurred
           scene with no colour floor at all, which is the same figure the
           arraylist, the watermark, the radar and the notifier all use, and
           what makes those read as glass rather than as tinted cards. */
        const al::al_tint tint{ 0.f, 0.f, 0.f, 0.45f };
        const float aa_pixels   = std::max(1.5f, 1.75f * g_settings.scale);
        g_glass.render(rects,
            display_w, display_h,
            bound_fb,
            bx, by, bw, bh,
            blur_passes,
            /*brightness=*/0.72f,
            tint,
            /*edge_softness=*/aa_pixels,
            /*shadow_radius=*/14.f * g_settings.scale,
            /*shadow_strength=*/0.10f,
            /*shadow_gamma=*/1.15f);
    }
}

namespace {

/* Controls are available with the client GUI or Minecraft chat open.
   The client panel and HUD editor retain priority over Spotify. */
bool interaction_allowed(float& mx, float& my)
{
    float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;

    const bool panel_open = dt::vgui::bridge::panelRect(px, py, pw, ph);
    if (!panel_open && !chat_open()) return false;

    if (dt::vgui::bridge::isHudEditorActive())
        return false;

    const ImVec2 m = ImGui::GetMousePos();
    mx = m.x; my = m.y;

    if (panel_open && mx >= px && my >= py && mx < px + pw && my < py + ph)
        return false;

    return true;
}

bool in_rect(float mx, float my, float x, float y, float w, float h)
{
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

/* @one transport mark, from the client's own icon atlas.

   These were hand-placed triangles and AddLine segments. At twelve pixels
   inside a thirty pixel button that stopped working: the seams between a
   polyline's segments showed as notches, because AddLine draws butt ends and
   knows nothing about the segment beside it, and the shuffle's two paths
   crossing in four pixels came out as an X rather than as two lanes swapping.

   The atlas exists precisely for this -- its own header says so. It rasterises
   the vector source AT THE SIZE ASKED FOR, so the antialiasing comes from the
   rasteriser rather than from minifying one big bake, and a mark's proportions
   live in one place instead of in a dozen literals at the call site.

   `px` is rounded because the atlas caches per integer size; asking for a
   fractional one every frame at a fractional panel scale would bake a new
   variant forever. */
void transport_icon(ImDrawList* dl, const char* name, float cx, float cy,
                    float px, ImU32 col)
{
    const int size = static_cast<int>(std::lround(px));
    if (size < 3) return;

    const unsigned long long tex = ::dt::vgui::bridge::iconTexture(name, size);
    if (!tex) return;

    /* Snapped to whole pixels. The bake is one texel per pixel, and landing it
       on a half pixel throws that away — the whole reason for baking per size
       in the first place. */
    const float h  = static_cast<float>(size) * 0.5f;
    const float x0 = std::round(cx - h);
    const float y0 = std::round(cy - h);

    dl->AddImage(static_cast<ImTextureID>(tex),
                 ImVec2(x0, y0), ImVec2(x0 + static_cast<float>(size),
                                        y0 + static_cast<float>(size)),
                 ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), col);
}

/* @a textured quad whose alpha varies down its height.

   AddImage takes one tint for the whole quad, so a fade had to be faked by
   drawing the image many times in thin strips of rising opacity -- and every
   boundary between two strips is a step, which is what showed up as lines
   ruled across the band. Writing the four vertices by hand gives the hardware
   the gradient to interpolate, so there is one draw and no edges at all. */
void image_fade_v(ImDrawList* dl, ImTextureID tex,
                  ImVec2 p0, ImVec2 p1, ImVec2 uv0, ImVec2 uv1,
                  int a_top, int a_bottom)
{
    const ImU32 c_top = IM_COL32(255, 255, 255, a_top);
    const ImU32 c_bot = IM_COL32(255, 255, 255, a_bottom);

    dl->PushTextureID(tex);
    dl->PrimReserve(6, 4);

    const unsigned int base = dl->_VtxCurrentIdx;

    dl->PrimWriteVtx(ImVec2(p0.x, p0.y), ImVec2(uv0.x, uv0.y), c_top);
    dl->PrimWriteVtx(ImVec2(p1.x, p0.y), ImVec2(uv1.x, uv0.y), c_top);
    dl->PrimWriteVtx(ImVec2(p1.x, p1.y), ImVec2(uv1.x, uv1.y), c_bot);
    dl->PrimWriteVtx(ImVec2(p0.x, p1.y), ImVec2(uv0.x, uv1.y), c_bot);

    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 0));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 0));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));

    dl->PopTextureID();
}

/* A round transport button: circle on hover, glyph always. Returns true on a
   fresh press inside it. */
bool transport_button(ImDrawList* dl, float cx, float cy, float r,
                      float mx, float my, bool live, float alpha)
{
    const bool hot = live && in_rect(mx, my, cx - r, cy - r, r * 2.f, r * 2.f);

    if (hot) {
        dl->AddCircleFilled(ImVec2(cx, cy), r,
            IM_COL32(255, 255, 255, static_cast<int>(38.f * alpha)), 24);
    }

    return hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

}

void render_text(int display_w, int display_h)
{
    if (!g_enabled || !g_layout.valid) return;

    auto& bold_pool = arraylist::shared_pool();
    auto& reg_pool  = regular_pool();
    if (!bold_pool.ensure()) return;
    reg_pool.ensure();   // best-effort — the artist label falls back to bold if this fails

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    dl->PushClipRect(ImVec2(0.f, 0.f),
        ImVec2(static_cast<float>(display_w), static_cast<float>(display_h)), false);

    const float x = g_layout.x;
    const float y = g_layout.y;
    const float w = g_layout.w;
    const float h = g_layout.h;

    const float dpi = std::max(1.f, static_cast<float>(display_h) / 1080.f);
    const float s = dpi * g_settings.scale;

    const float radius_base = 6.f * g_settings.scale * g_settings.rounding;
    const float cap = std::max(0.f, std::min(w, h) * 0.5f - 1.f);
    const float card_rad = std::min(radius_base, cap);

    track_info track;
    {
        std::lock_guard<std::mutex> lk(g_track_mx);
        track = g_track;
    }

    const bool emberReady=g_settings.background==1 && track.ember_tex_id && !track.title.empty();
    // Ember supplies its own opaque fill and shaded antialiased edge.
    if(!emberReady) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
        IM_COL32(14, 14, 18, 255), card_rad);

    if (track.title.empty()) {
        // Compact empty state: no artwork, transport, seek targets or lyrics.
        g_eff_pos_ms = 0;
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
            IM_COL32(255, 255, 255, 18), card_rad);
        const float art_x = x + 12.f * s, art_y = y + 12.f * s;
        dl->AddRectFilled(ImVec2(art_x, art_y), ImVec2(art_x + 44.f*s, art_y + 44.f*s),
            IM_COL32(25, 25, 29, 255), 6.f*s);
        const ImU32 note = IM_COL32(150, 150, 158, 255);
        const float nx = art_x + 17.f*s, ny = art_y + 15.f*s;
        dl->AddLine(ImVec2(nx, ny+13.f*s), ImVec2(nx, ny), note, 1.5f*s);
        dl->AddLine(ImVec2(nx, ny), ImVec2(nx+11.f*s, ny-3.f*s), note, 1.5f*s);
        dl->AddLine(ImVec2(nx+11.f*s, ny-3.f*s), ImVec2(nx+11.f*s, ny+10.f*s), note, 1.5f*s);
        dl->AddCircle(ImVec2(nx-2.f*s, ny+14.f*s), 3.f*s, note, 16, 1.5f*s);
        dl->AddCircle(ImVec2(nx+9.f*s, ny+11.f*s), 3.f*s, note, 16, 1.5f*s);
        auto label = [&](bool bold, float size, ImVec2 pos, ImU32 color, const char* text) {
            ImFont* font = bold ? bold_pool.get(size) : reg_pool.get(size);
            const bool fallback = !font;
            if (fallback) font = bold_pool.get(size);
            if (!font) return;
            dl->PushTextureID((bold || fallback) ? bold_pool.texture_id() : reg_pool.texture_id());
            dl->AddText(font, size, pos, color, text);
            dl->PopTextureID();
        };
        label(true, 11.f*s, ImVec2(x+65.f*s,y+21.f*s), IM_COL32(240,240,244,255), "Not playing");
        label(false, 10.f*s, ImVec2(x+65.f*s,y+36.f*s), IM_COL32(150,150,158,255), "Play something to get started");
        dl->AddRectFilled(ImVec2(x+12.f*s,y+67.f*s), ImVec2(x+w-12.f*s,y+71.f*s),
            IM_COL32(58,58,62,255), 2.f*s);
        label(false, 12.f*s, ImVec2(x+12.f*s,y+78.f*s), IM_COL32(100,100,106,255), "0:00");
        label(false, 12.f*s, ImVec2(x+w-45.f*s,y+78.f*s), IM_COL32(100,100,106,255), "-0:00");
        dl->PopClipRect();
        return;
    }

    const unsigned int styled = g_settings.background==1?track.ember_tex_id:
        g_settings.background==2?track.silk_tex_id:0;
    const unsigned int backdrop = styled ? styled : (track.blurred_bg_tex_id
        ? track.blurred_bg_tex_id : track.thumb_tex_id);
    if (backdrop) {
        const float dst_aspect = w / std::max(1.f, h);
        ImVec2 uv0(0.f, 0.f), uv1(1.f, 1.f);
        if (dst_aspect > 1.f) {
            const float v_span = 1.f / dst_aspect;
            uv0.y = 0.5f - v_span * 0.5f;
            uv1.y = 0.5f + v_span * 0.5f;
        } else if (dst_aspect < 1.f) {
            const float u_span = dst_aspect;
            uv0.x = 0.5f - u_span * 0.5f;
            uv1.x = 0.5f + u_span * 0.5f;
        }
        if (styled) { uv0={0,0}; uv1={1,1}; }
        // Fill the rounded card with the cover; the scrim supplies contrast.
        if(g_settings.background==1 && styled) {
            draw_ember(dl,static_cast<ImTextureID>(backdrop),x,y,w,h,card_rad,
                (255.f-70.f*std::max(g_wide_anim,g_cover_anim))/255.f);
        } else {
            dl->AddImageRounded(static_cast<ImTextureID>(backdrop),
                ImVec2(x, y), ImVec2(x + w, y + h),
                uv0, uv1, IM_COL32(255, 255, 255, 255), card_rad);
        }

        if(!emberReady) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
            IM_COL32(8, 10, 16, 20), card_rad);
    } else {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
            IM_COL32(20, 20, 26, 170), card_rad);
    }

    /* Scrim for text legibility. AddRectFilledMultiColor can't round its
       corners, and stacking two rounded rects at different heights leaves
       a visible seam where the top edge of the darker one meets the
       lighter one. Solution: a pre-baked 1x128 vertical gradient texture
       uploaded once, drawn with AddImageRounded so it inherits the
       card's rounded silhouette AND blends smoothly top-to-bottom with
       no seams. */
    {
        static std::atomic<unsigned int> s_scrim_tex{0};
        if (s_scrim_tex.load(std::memory_order_acquire) == 0) {
            std::vector<std::uint8_t> px(128 * 4, 0);
            for (int i = 0; i < 128; ++i) {
                const float t = i / 127.f;
                /* A smooth scrim preserves color above and protects controls below. */
                const float a = 0.10f + 0.30f * (t * t * (3.f - 2.f * t));
                px[i * 4 + 3] = static_cast<std::uint8_t>(a * 255.f);
            }
            queue_texture_upload(std::move(px), 1, 128, &s_scrim_tex);
        }
        const unsigned int tex = s_scrim_tex.load(std::memory_order_acquire);
        if (emberReady) {
            // Already shaded in the mesh, including its transparent fringe.
        } else if (tex) {
            /* @lighter in the shapes whose subject IS the artwork.

               The scrim was baked for the small player, where it is the only
               thing between white text and a photograph. In the wide and cover
               shapes it was doing that job over a card three times the area and
               flattening the record into a grey wash -- the background is
               supposed to be the sleeve, out of focus. Modulated rather than
               re-baked: the curve is right, there is just less of it. */
            dl->AddImageRounded(static_cast<ImTextureID>(tex),
                ImVec2(x, y), ImVec2(x + w, y + h),
                ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                IM_COL32(255, 255, 255,
                         static_cast<int>(255.f - 70.f * std::max(g_wide_anim, g_cover_anim))),
                card_rad);
        } else {
            /* Uniform fallback while the texture is still uploading. */
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                IM_COL32(0, 0, 0, 48), card_rad);
        }
    }

    /* Thumbnail — crisp square cover on the left. */
    /* @can the card be touched this frame, and where is the pointer.

       Resolved once so the transport row and the seek bar agree, and so the
       hover states below do not light up when the click could never land. */
    float mx = -1.f, my = -1.f;
    const bool can_touch = interaction_allowed(mx, my);
    const bool over_card = can_touch && in_rect(mx, my, x, y, w, h);
    const bool live      = can_touch;

    /* @the artwork travels from the corner to the whole card.

       One rect lerped between the two, rather than two code paths: a crossfade
       between a small cover and a large one shows both at once halfway through,
       which looks like a bug. */
    const float ca = g_cover_anim;
    const float wa = g_wide_anim;

    const float thumb_pad  = 8.f * s * (1.f - ca);
    /* The player is the fixed block at the top; when the lyrics panel is open
       the card is taller than this, and everything the player owns still
       measures from here. Must match render_background. */
    const float player_h   = (96.f + 36.f * g_ctl_reveal) * s;

    /* The art sizes off the TOP band, not the whole card — the control row
       owns the bottom of the player and the cover must not stretch into it. */
    const float top_band   = 96.f * s;

    const float small_sz   = top_band - 8.f * s * 2.f;
    const float wide_sz    = 200.f * s;

    /* Three destinations for one rect: the corner thumbnail, the full card, and
       the big square on the left of the wide view. */
    const float base_sz    = small_sz + (wide_sz - small_sz) * wa;

    // Center the player in the full wide card when lyrics are hidden.
    // Slide it left as the lyric column appears.
    const float wide_middle = (560.f * s - wide_sz) * 0.5f;
    const float wide_left   = wide_middle + (20.f * s - wide_middle) * g_words_anim;

    /* The two axes part company here. The vertical inset is the card's own top
       padding and does not care about the column; the horizontal one is where
       the art sits INSIDE that column. They were one expression written twice,
       which is what made it look like they had to stay equal. */
    const float thumb_off_y = thumb_pad + (20.f * s - 8.f * s) * wa * (1.f - ca);
    const float thumb_off_x = thumb_pad + (wide_left - 8.f * s) * wa * (1.f - ca);

    /* @the square is FITTED to the card, not merely sized for it.

       thumb_size lerps towards the card's WIDTH while the card's HEIGHT lerps
       towards cover_h, and the two only agree once both animations have
       finished. g_wide_anim and g_cover_anim are independent eased floats, so
       going straight from the wide view to the cover view leaves both non-zero
       for about half a second -- and in that window the square is taller than
       the card it is supposed to be filling. Nothing clips it (the only rect on
       the stack is the whole screen, and the card draws no clip of its own), so
       it hung past the bottom edge and the band and the rounded cap painted a
       flat line straight across the artwork.

       Fitted rather than clipped, deliberately: a clip would cut the square
       against the card's rounded corners, which is the same square edge one
       layer down. */
    const float thumb_size = std::min(base_sz + (w - base_sz) * ca,
                                      std::max(0.f, std::min(w - thumb_off_x,
                                                             h - thumb_off_y)));

    const float thumb_x    = x + thumb_off_x;
    const float thumb_y    = y + thumb_off_y;
    const float thumb_rad  = 5.f * s + (card_rad - 5.f * s) * ca;

    if (track.thumb_tex_id) {
        dl->AddImageRounded(static_cast<ImTextureID>(track.thumb_tex_id),
            ImVec2(thumb_x, thumb_y),
            ImVec2(thumb_x + thumb_size, thumb_y + thumb_size),
            ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
            IM_COL32(255, 255, 255, 255),
            thumb_rad);
    } else {
        dl->AddRectFilled(
            ImVec2(thumb_x, thumb_y),
            ImVec2(thumb_x + thumb_size, thumb_y + thumb_size),
            IM_COL32(38, 38, 48, 230), thumb_rad);
    }

    /* Text — title (bold) + artist (regular). */
    /* In cover mode the words leave the right-hand column and sit on the band
       across the bottom of the artwork. */
    /* @the band has to hold four rows, so it is sized for four.

       At ninety-six pixels the title, the artist, the progress row and the
       transport were stacked into each other and the title came out squeezed
       into whatever was left over. */
    const float band_top   = y + h - 116.f * s;
    const float stack_text_l = thumb_x + thumb_size + 10.f * s;
    /* Under the art's own left edge, wherever the line above put it. */
    const float wide_text_l   = thumb_x;

    const float text_left  = (stack_text_l * (1.f - wa) + wide_text_l * wa) * (1.f - ca)
                           + (x + 20.f * s) * ca;
    // Keep titles within the artwork column, including when lyrics are visible.
    const float text_right = (x + w - 10.f * s) * (1.f - wa)
                           + (thumb_x + thumb_size) * wa;

    /* A shade larger in cover mode: it is the only text on a full-bleed
       photograph and has to hold its own against it. */
    /* @the band the player sits on in cover mode.

       The blurred copy of the album art is already loaded for the card
       background, so the band is that same texture over the bottom of the card
       with a gradient into it -- the cover blurred over itself, rather than a
       grey bar laid on top of a picture. */
    if (ca > 0.01f) {
        const float bt = band_top;
        const float bb = y + h;

        /* @the band is the cover seen through frosted glass, so it has to be
           the SAME cover, in the SAME place.

           The first cut stretched a 28% slice of the blurred texture across the
           band. That is a different picture from the one above it -- magnified,
           misaligned, and with the blur's own softness blown up with it, which
           is what made it read as a grey smear rather than as glass. Drawn over
           the FULL card rect and clipped to the band instead: every texel lands
           exactly where its sharp twin is, so the band looks like the cover
           going out of focus rather than like something laid on top of it. */
        /* The gentler copy. The card's own backdrop texture is blurred until
           there is nothing left of the picture, which is right behind a small
           card and wrong as glass over the artwork itself. */
        const unsigned int band_src = track.band_tex_id ? track.band_tex_id
                                                        : track.blurred_bg_tex_id;

        if (band_src) {
            const ImTextureID bg = static_cast<ImTextureID>(band_src);

            /* Aspect-correct against the card, matching the sharp art above. */
            const float dst_aspect = w / std::max(1.f, h);
            ImVec2 uv0(0.f, 0.f), uv1(1.f, 1.f);
            if (dst_aspect > 1.f) {
                const float v_span = 1.f / dst_aspect;
                uv0.y = 0.5f - v_span * 0.5f;
                uv1.y = 0.5f + v_span * 0.5f;
            } else if (dst_aspect < 1.f) {
                const float u_span = dst_aspect;
                uv0.x = 0.5f - u_span * 0.5f;
                uv1.x = 0.5f + u_span * 0.5f;
            }

            /* @the top edge dissolves instead of cutting.

               A hard line across a photograph is the one thing that makes an
               overlay look pasted on. The blur is brought in over a couple of
               dozen pixels with a single gradient-shaded quad -- the strip
               version this replaces stepped its opacity ten times and every
               step showed up as a rule across the band. */
            const float feather = 44.f * s;
            const int   full    = static_cast<int>(255.f * ca);

            /* UVs for the two pieces, sliced out of the card-wide mapping so
               the blurred copy stays registered with the sharp image. */
            auto v_at = [&](float py) {
                return uv0.y + (uv1.y - uv0.y) * std::clamp((py - y) / std::max(1.f, h), 0.f, 1.f);
            };

            const float fmid = std::min(bt + feather, bb);

            image_fade_v(dl, bg,
                ImVec2(x, bt), ImVec2(x + w, fmid),
                ImVec2(uv0.x, v_at(bt)), ImVec2(uv1.x, v_at(fmid)),
                0, full);

            /* The rest at full strength, clipped so the bottom corners follow
               the card rather than running square past it. */
            dl->PushClipRect(ImVec2(x, fmid), ImVec2(x + w, bb), true);
            dl->AddImageRounded(bg, ImVec2(x, y), ImVec2(x + w, y + h),
                uv0, uv1, IM_COL32(255, 255, 255, full), card_rad);
            dl->PopClipRect();
        }

        /* @the ramp stops short of the corners, and a rounded cap closes it.

           AddRectFilledMultiColor cannot round, so running it to the bottom
           edge painted two square corners over the card's rounded ones -- the
           spikes. The gradient now ends a radius above the bottom and the last
           strip is a rounded rect, which is the only shape that can finish a
           rounded card. Lighter overall too, so the progress row reads through
           it instead of being buried. */
        {
            const float cap  = std::max(card_rad, 1.f);
            const float stop = bb - cap;
            const float mid  = bt + (stop - bt) * 0.5f;

            const ImU32 c0 = IM_COL32(0, 0, 0, 0);
            const ImU32 c1 = IM_COL32(0, 0, 0, static_cast<int>(34.f * ca));
            const ImU32 c2 = IM_COL32(0, 0, 0, static_cast<int>(74.f * ca));

            if (stop > bt) {
                dl->AddRectFilledMultiColor(ImVec2(x, bt), ImVec2(x + w, mid),
                                            c0, c0, c1, c1);
                dl->AddRectFilledMultiColor(ImVec2(x, mid), ImVec2(x + w, stop),
                                            c1, c1, c2, c2);
            }

            /* @butted against the ramp, never overlapping it.

               Starting the cap half a radius early meant the ramp and the cap
               were both painting those pixels, and two passes of the same
               translucent black is a darker strip than either -- the black line
               that showed up across the bottom of some covers. Meeting exactly
               at `stop`, where the ramp has already reached c2, the join is
               continuous. */
            // Clip the full-height rounded card so its corner radius matches
            // the artwork exactly; a short cap gets its radius clamped by ImGui.
            dl->PushClipRect(ImVec2(x, stop), ImVec2(x + w, bb), true);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, bb), c2, card_rad);
            dl->PopClipRect();
        }
    }

    const float title_size  = 14.f * s + 3.f * s * ca;
    const float artist_size = 11.f * s;

    ImFont* title_f  = bold_pool.get(title_size);
    ImFont* artist_f = reg_pool.get(artist_size);
    if (!artist_f) artist_f = bold_pool.get(artist_size);   // fallback if proxima.ttf missing
    if (!title_f) { dl->PopClipRect(); return; }

    const float stack_title_y = y + top_band * 0.32f - title_size * 0.5f;
    /* Directly under the art, with the progress row below the pair — the order
       a player reads in. */
    const float wide_title_y   = thumb_y + thumb_size + 12.f * s;

    const float title_y = (stack_title_y * (1.f - wa) + wide_title_y * wa) * (1.f - ca)
                        + (band_top + 14.f * s) * ca;

    auto draw_ellipsized = [&](ImFont* f, float sz, ImVec2 pos, ImU32 col,
                               const std::string& text, float max_w)
    {
        if (text.empty()) return;
        const char* begin = text.c_str();
        const char* end   = begin + text.size();
        ImVec2 full = f->CalcTextSizeA(sz, FLT_MAX, -1.f, begin, end);
        if (full.x <= max_w) {
            dl->AddText(f, sz, pos, col, begin, end);
            return;
        }
        const char* ell = "...";
        ImVec2 ell_sz = f->CalcTextSizeA(sz, FLT_MAX, -1.f, ell);
        const float allowed = std::max(0.f, max_w - ell_sz.x);
        size_t lo = 0, hi = text.size();
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            ImVec2 sz2 = f->CalcTextSizeA(sz, FLT_MAX, -1.f, begin, begin + mid);
            if (sz2.x <= allowed) lo = mid; else hi = mid - 1;
        }
        dl->AddText(f, sz, pos, col, begin, begin + lo);
        dl->AddText(f, sz, ImVec2(pos.x + f->CalcTextSizeA(sz, FLT_MAX, -1.f,
                                    begin, begin + lo).x, pos.y),
                    col, ell);
    };

    const float text_max_w = text_right - text_left;

    /* Push the atlas each font belongs to before its own draw — the two
       pools use different GL textures, so a single push would sample the
       wrong glyph pixels. */
    dl->PushTextureID(bold_pool.texture_id());

    draw_ellipsized(title_f, title_size,
                    ImVec2(text_left, title_y),
                    IM_COL32(255, 255, 255, 252),
                    track.title, text_max_w);
    dl->PopTextureID();

    const float artist_y = title_y + title_size + 2.f * s;
    const ImTextureID artist_tex = (artist_f == title_f)
        ? bold_pool.texture_id() : reg_pool.texture_id();
    dl->PushTextureID(artist_tex);
    draw_ellipsized(artist_f, artist_size,
                    ImVec2(text_left, artist_y),
                    IM_COL32(215, 215, 225, 205),
                    track.artist, text_max_w);
    dl->PopTextureID();

    /* Progress + times, all on one horizontal row: [elapsed] [bar] [total].
       No play-head dot — the pill fill alone is enough and the dot bled
       past the pill edge at the fill boundary. */
    /* @the clock runs whether or not the bar is drawn.

       All of this lived inside the progress-bar branch, so turning the bar off
       froze g_eff_pos_ms at whatever it last held -- and the lyrics panel reads
       that same variable, so its highlight stopped following the song
       altogether. The clock is state; only the drawing below is optional.

       The statics come out with it: the seek handler further down assigns
       s_display_ms from inside the drawing branch. */
    static std::string s_clock_key;
    static double      s_display_ms   = 0.0;
    static long long   s_seen_sample  = 0;
    static long long   s_last_frame_ns = 0;
    static bool        s_was_playing = false;

    long long eff_pos = 0;
    {
        // Advance between samples, then accept timeline corrections immediately.
        // Smoothing corrections made lyrics lag after seeks and stale samples.
        const std::string key    = track.title + "" + track.artist;
        const long long   now_ns = nowNanos();

        const bool new_track = (key != s_clock_key);
        if (new_track) {
            s_clock_key    = key;
            s_display_ms   = static_cast<double>(track.position_ms);
            s_seen_sample  = track.snapshot_ns;
            s_last_frame_ns = now_ns;
        }

        /* Free-run between samples. */
        {
            const long long dn = now_ns - s_last_frame_ns;
            s_last_frame_ns = now_ns;

            if (track.playing && dn > 0)
                s_display_ms += static_cast<double>(dn) / 1'000'000.0;
        }

        /* Correct against a sample, exactly once per sample. */
        if (new_track || (track.snapshot_ns > 0 && track.snapshot_ns != s_seen_sample)
            || track.playing != s_was_playing) {
            s_seen_sample = track.snapshot_ns;

            double target = static_cast<double>(track.position_ms);
            if (track.playing && track.snapshot_ns > 0) {
                const long long age = now_ns - track.snapshot_ns;
                if (age > 0) target += static_cast<double>(age) / 1'000'000.0;
            }

            s_display_ms = target;
        }
        s_was_playing = track.playing;

        if (s_display_ms < 0.0) s_display_ms = 0.0;
        if (track.duration_ms > 0 && s_display_ms > static_cast<double>(track.duration_ms))
            s_display_ms = static_cast<double>(track.duration_ms);

        eff_pos = static_cast<long long>(s_display_ms);

        /* The panel below follows this same clock. Reading SMTC's last poll
           instead would put the highlight up to half a second behind the
           vocal, which is the whole thing a synced view is for. */
        g_eff_pos_ms = eff_pos;
    }

    if (g_settings.show_progress) {
        /* Format times first so we know how wide they are. */
        char t_now[16]; char t_total[16];
        format_time_ms(eff_pos,           t_now,   sizeof(t_now));
        format_time_ms(track.duration_ms, t_total, sizeof(t_total));

        const float time_size = 11.f * s;
        ImFont* time_f = reg_pool.get(time_size);
        if (!time_f) time_f = bold_pool.get(time_size);
        if (!time_f) { dl->PopClipRect(); return; }
        const ImTextureID time_tex = (time_f == bold_pool.get(time_size))
            ? bold_pool.texture_id() : reg_pool.texture_id();

        const ImVec2 now_sz   = time_f->CalcTextSizeA(time_size, FLT_MAX, -1.f, t_now);
        const ImVec2 total_sz = time_f->CalcTextSizeA(time_size, FLT_MAX, -1.f, t_total);

        const float prog_h   = std::max(6.f, 7.f * s);
        const float prog_r   = prog_h * 0.5f;
        const float row_h    = std::max(prog_h, time_size);
        /* The progress row sits at the bottom of the top band; the control
           row is what actually closes the card. */
        const float ctl_h    = 34.f * s;
        const float stack_row_y = y + top_band - row_h - 12.f * s;
        /* @below the artwork, not across its bottom edge.

           It was sitting on the last few rows of the cover, which put white
           text and a white bar over whatever the record happened to be. The
           card has room under the art; the row belongs there. */
        const float wide_row_y   = thumb_y + thumb_size + 58.f * s;

        const float row_y    = (stack_row_y * (1.f - wa) + wide_row_y * wa) * (1.f - ca)
                             + (y + h - 70.f * s) * ca;
        const float prog_y   = row_y + (row_h - prog_h) * 0.5f;
        const float time_y   = row_y + (row_h - time_size) * 0.5f - 1.f;

        const float gap      = 10.f * s;
        const float row_left  = text_left;
        /* @in the wide view the row belongs to the ARTWORK column.

           At full card width it ran on underneath the lyrics and parked the
           total time in the far corner of a panel it has nothing to do with,
           which read as a rule cutting the card in half. */
        /* @the wide row gets the artwork column plus its margins.

           Ending it at the artwork's right edge left the bar sharing 200 px
           with two timestamps, so it came out a stub. Running to the same
           inset the art starts from gives it the whole column. */
        const float row_right = (x + w - 10.f * s) * (1.f - wa)
                              + (thumb_x + thumb_size + 10.f * s) * wa;
        const float bar_x0    = row_left + now_sz.x + gap;
        const float bar_x1    = row_right - total_sz.x - gap;
        const float total_x   = row_right - total_sz.x;

        /* Times on both sides. */
        dl->PushTextureID(time_tex);
        dl->AddText(time_f, time_size, ImVec2(row_left, time_y),
                    IM_COL32(225, 225, 230, 220), t_now);
        dl->AddText(time_f, time_size, ImVec2(total_x, time_y),
                    IM_COL32(225, 225, 230, 220), t_total);
        dl->PopTextureID();

        /* @the bar answers the mouse.

           The track is seven pixels tall, which is not a drag target, so the
           hit rect is padded well past it vertically. The drag is tracked with
           a latch rather than read per frame: letting go outside the bar must
           still commit the seek, and re-entering must not start a new one. */
        static bool  s_seek_drag = false;
        static float s_seek_frac = 0.f;

        if (bar_x1 - bar_x0 > prog_h * 2.f && live && track.can_seek
            && track.duration_ms > 0)
        {
            const bool over = in_rect(mx, my, bar_x0, prog_y - 9.f * s,
                                      bar_x1 - bar_x0, prog_h + 18.f * s);

            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                s_seek_drag = true;

            if (s_seek_drag) {
                s_seek_frac = std::clamp((mx - bar_x0) / (bar_x1 - bar_x0), 0.f, 1.f);

                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    s_seek_drag = false;

                    const long long want =
                        static_cast<long long>(s_seek_frac * track.duration_ms);

                    push_command(cmd::seek, want);

                    /* Move the local clock now. Otherwise the bar snaps back
                       to the old spot and sits there until SMTC catches up half
                       a second later, which reads as the seek having failed.

                       The sample counter is left alone on purpose: the next
                       sample still describes the old position, and letting the
                       correction see it would drag the bar backwards once
                       before the player has moved. It is more than the snap
                       threshold away, so it snaps forward again when the real
                       one lands. */
                    s_display_ms = static_cast<double>(want);
                    eff_pos      = want;
                }
            }
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            s_seek_drag = false;
        }

        /* Bar between them — only if there's room. */
        if (bar_x1 - bar_x0 > prog_h * 2.f) {
            /* Over a full-bleed cover a 27% white track disappears into
               anything pale. In that shape it becomes a dark trough instead,
               which reads on light and dark art alike. */
            const int track_a = static_cast<int>(70.f + 60.f * ca);
            dl->AddRectFilled(ImVec2(bar_x0, prog_y),
                              ImVec2(bar_x1, prog_y + prog_h),
                              ca > 0.02f ? IM_COL32(0, 0, 0, track_a)
                                         : IM_COL32(255, 255, 255, 70),
                              prog_r);

            double frac = 0.0;
            if (track.duration_ms > 0)
                frac = std::clamp(
                    static_cast<double>(eff_pos) /
                    static_cast<double>(track.duration_ms), 0.0, 1.0);

            /* While dragging the bar follows the cursor, not the clock. */
            if (s_seek_drag) frac = s_seek_frac;

            const float fill_x = bar_x0 + static_cast<float>(frac) * (bar_x1 - bar_x0);

            if (fill_x > bar_x0 + prog_r) {
                dl->AddRectFilled(ImVec2(bar_x0, prog_y),
                                  ImVec2(fill_x, prog_y + prog_h),
                                  IM_COL32(255, 255, 255, 245),
                                  prog_r);
            }

            /* The knob only exists while the bar can actually be moved, so it
               is the affordance as well as the handle. */
            if (live && track.can_seek && track.duration_ms > 0) {
                const bool near_bar = s_seek_drag
                    || in_rect(mx, my, bar_x0, prog_y - 9.f * s,
                               bar_x1 - bar_x0, prog_h + 18.f * s);

                if (near_bar) {
                    dl->AddCircleFilled(ImVec2(fill_x, prog_y + prog_r),
                        prog_h * (s_seek_drag ? 1.05f : 0.9f),
                        IM_COL32(255, 255, 255, 255), 20);
                }
            }
        }
    }

    /* ── the artwork's own affordance ────────────────────────────────────── */

    /* @the cover says it can be opened, but only when asked.

       A permanent badge on the album art is clutter on something whose whole
       job is to be looked at. It fades in under the pointer and fades out with
       it, on the same curve the rest of the card animates on. */
    {
        const bool over_art = live && in_rect(mx, my, thumb_x, thumb_y,
                                              thumb_size, thumb_size);

        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
        const float want = (over_art && !g_cover) ? 1.f : 0.f;
        g_art_hint += (want - g_art_hint) * std::clamp(12.f * dt, 0.f, 1.f);

        if (over_art && !g_cover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            set_view(view::cover);

        if (g_art_hint > 0.004f) {
            const float a  = g_art_hint;
            const float cx = thumb_x + thumb_size * 0.5f;
            const float cy = thumb_y + thumb_size * 0.5f;
            const float r  = thumb_size * 0.22f;

            dl->AddRectFilled(ImVec2(thumb_x, thumb_y),
                ImVec2(thumb_x + thumb_size, thumb_y + thumb_size),
                IM_COL32(0, 0, 0, static_cast<int>(120.f * a)), thumb_rad);

            dl->AddCircleFilled(ImVec2(cx, cy), r,
                IM_COL32(0, 0, 0, static_cast<int>(90.f * a)), 24);

            const ImU32 ink = IM_COL32(255, 255, 255, static_cast<int>(235.f * a));
            const float e  = r * 0.52f;
            const float th = std::max(1.f, 1.8f * s);

            dl->AddLine(ImVec2(cx - e, cy - e), ImVec2(cx - e * 0.2f, cy - e), ink, th);
            dl->AddLine(ImVec2(cx - e, cy - e), ImVec2(cx - e, cy - e * 0.2f), ink, th);
            dl->AddLine(ImVec2(cx + e, cy + e), ImVec2(cx + e * 0.2f, cy + e), ink, th);
            dl->AddLine(ImVec2(cx + e, cy + e), ImVec2(cx + e, cy + e * 0.2f), ink, th);
        }
    }

    /* ── back out of cover mode ───────────────────────────────────────────── */

    /* @quiet on purpose.

       It sits on top of the artwork, so anything with weight would be a mark on
       the picture. It brightens under the pointer and is otherwise barely
       there -- which is enough, because there is only one way out and this is
       where every viewer already looks for it. */
    if (g_cover_anim > 0.01f && live) {
        const float bx = x + 20.f * s;
        const float by = y + 20.f * s;
        const float r  = 13.f * s;

        const bool hot = live && in_rect(mx, my, bx - r, by - r, r * 2.f, r * 2.f);

        if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            set_view(view::player);

        const int base = static_cast<int>((hot ? 225.f : 120.f) * g_cover_anim);
        const ImU32 ink = IM_COL32(255, 255, 255, base);
        const float e = r * 0.42f;
        const float th = std::max(1.f, 1.9f * s);

        if (hot) {
            dl->AddCircleFilled(ImVec2(bx, by), r,
                IM_COL32(0, 0, 0, static_cast<int>(80.f * g_cover_anim)), 24);
        }

        /* A left-pointing chevron. */
        dl->AddLine(ImVec2(bx + e * 0.55f, by - e), ImVec2(bx - e * 0.55f, by), ink, th);
        dl->AddLine(ImVec2(bx - e * 0.55f, by), ImVec2(bx + e * 0.55f, by + e), ink, th);
    }

    /* ── control row ─────────────────────────────────────────────────────── */

    /* @laid out, not overlaid.

       Transport centred, because that is what the eye goes to; expand pinned
       left and lyrics pinned right, because they are modes rather than actions
       and modes belong at the edges. Always drawn: a control that only exists
       while the pointer is on it cannot be found by someone looking for it. */
    {
        /* @a control you cannot press should not be drawn.

           With the panel closed the overlay receives no mouse at all, so the
           transport was painting buttons that were pure decoration -- and
           worse, that implied the card was interactive when it is not. Eased
           rather than switched, so closing the panel does not make them vanish
           between two frames. */
        /* The same value the layout sized the card with — two animations would
           drift and the buttons would fade against a box of the wrong height. */
        const float cr = g_ctl_reveal;

        /* Anchored to the PLAYER block, not to the card. Anchoring it to the
           card sent the whole row to the bottom of the tall version, with the
           lyrics between it and the song it was controlling. */
        const float stack_ctl = y + player_h - 34.f * s * 0.5f - 6.f * s;
        const float wide_ctl  = y + h - 26.f * s;

        const float ctl_cy = (stack_ctl * (1.f - wa) + wide_ctl * wa) * (1.f - ca)
                           + (y + h - 28.f * s) * ca;
        const float cx     = x + w * 0.5f;
        const float step   = 34.f * s;
        const float r      = 15.f * s;
        /* @the mark is 17 of the button's 30 pixels.

           It was twelve, and that is why the shuffle never read: a crossing
           needs room. Seventeen inside thirty is the proportion Spotify's own
           transport uses, and the atlas bakes at exactly this many pixels. */
        const float ic     = 17.f * s;         /* transport mark, pixels */

        const ImU32 ink    = IM_COL32(255, 255, 255, static_cast<int>(235.f * cr));
        const ImU32 ink_lo = IM_COL32(255, 255, 255, static_cast<int>(150.f * cr));

        if (cr > 0.004f) {

        /* @shuffle, left of the group.

           Off to the side because it is a mode and the other three are actions
           -- the same reason the expand and lyrics controls live at the edges.
           Dimmed until the session says it is on. */
        if (track.can_shuffle) {
            if (transport_button(dl, cx - step * 2.f, ctl_cy, r, mx, my, live, 1.f))
                push_command(cmd::shuffle, track.shuffled ? 0 : 1);

            transport_icon(dl, "tr_shuffle", cx - step * 2.f, ctl_cy, ic,
                           track.shuffled ? ink : ink_lo);
        }

        /* previous */
        if (transport_button(dl, cx - step, ctl_cy, r, mx, my, live, 1.f))
            push_command(cmd::prev);
        transport_icon(dl, "tr_prev", cx - step, ctl_cy, ic, ink);

        /* play / pause */
        if (transport_button(dl, cx, ctl_cy, r * 1.15f, mx, my, live, 1.f)) {
            push_command(cmd::toggle_play);
            assume_playing(!track.playing);
        }

        /* Play sits a shade larger than the skips, the way it does in every
           player: it is the one control the eye should land on first. */
        transport_icon(dl, track.playing ? "tr_pause" : "tr_play",
                       cx, ctl_cy, ic * 1.12f, ink);

        /* next */
        if (transport_button(dl, cx + step, ctl_cy, r, mx, my, live, 1.f))
            push_command(cmd::next);
        transport_icon(dl, "tr_next", cx + step, ctl_cy, ic, ink);

        /* @expand, pinned left -- and absent in cover mode.

           There is nothing left to expand into once the artwork already fills
           the card, and the back chevron occupies that corner. */
        const float side_x = x + 20.f * s;

        if (ca < 0.5f) {
            if (transport_button(dl, side_x, ctl_cy, r, mx, my, live, 1.f))
                set_view(g_wide ? view::player : view::wide);
        transport_icon(dl, "tr_expand", side_x, ctl_cy, ic, ink_lo);
        }

        /* @the lyrics toggle rides up to the corner in cover mode.

           Down on the transport line it would be sitting on the artwork's own
           band next to three buttons it has nothing to do with. Top right is
           where a mode switch belongs on a full-bleed image, and it is the
           corner the back chevron does not use. */
        const float aa_x = x + w - 20.f * s;
        const float aa_y = ctl_cy * (1.f - ca) + (y + 22.f * s) * ca;

        const auto lyrics_state = get_lyrics();
        const bool lyrics_available = !lyrics_state || !lyrics_state->empty();
        if (lyrics_available && transport_button(dl, aa_x, aa_y, r, mx, my, live, 1.f)) {
            /* Turning the words on in a shape that cannot hold them means the
               player shape, which can. The wide shape can hold them itself, so
               it stays put and simply grows its second column. */
            if (!g_lyrics_open && g_cover) set_view(view::player);
            g_lyrics_open = !g_lyrics_open;
        }
        {
            const float aa_size = 13.f * s;
            ImFont* aa_f = bold_pool.get(aa_size);
            if (aa_f) {
                const ImVec2 sz = aa_f->CalcTextSizeA(aa_size, FLT_MAX, -1.f, "Aa");
                dl->PushTextureID(bold_pool.texture_id());
                dl->AddText(aa_f, aa_size,
                    ImVec2(aa_x - sz.x * 0.5f, aa_y - sz.y * 0.5f),
                    !lyrics_available ? IM_COL32(255, 255, 255, static_cast<int>(55.f * cr))
                        : (g_lyrics_open ? ink : ink_lo), "Aa");
                dl->PopTextureID();

                /* @a dot, and a quiet one.

                   An underline reads as a heading rule at this size and cuts
                   the row in half. The mark only has to answer "is this on",
                   which a dot does without competing with the transport a few
                   pixels away. */
                if (g_lyrics_open) {
                    dl->AddCircleFilled(
                        ImVec2(aa_x, aa_y + sz.y * 0.5f + 3.5f * s),
                        std::max(1.f, 1.6f * s),
                        IM_COL32(190, 190, 196, static_cast<int>(165.f * cr)), 10);
                }
            }
        }

        }
    }

    /* ── lyrics ──────────────────────────────────────────────────────────── */

    /* @the words go beside the art, or under it, on one code path.

       The column is a rect, and where that rect lives is the only thing the two
       views disagree about. Writing them as two blocks would have meant two
       copies of the follow logic, drifting apart the first time either changed. */
    /* wa alone put the words on screen for the whole of the wide view. Scaled
       by g_words_anim, the column is present exactly when the toggle and the
       sheet both say so -- and it fades with the same easing the card resizes
       on, so the two cannot disagree mid-animation. */
    const float lyr_present = std::max(g_lyrics_anim, wa * g_words_anim) * (1.f - ca);

    if (lyr_present > 0.002f) {
        const std::shared_ptr<const lyrics_doc> doc = get_lyrics();

        /* @a column, not the full width.

           Lyrics running the whole card read as a paragraph; inset they read as
           lines, which is what they are. It also gives the fade at the edges
           somewhere to happen. */
        /* Stacked: full width under the player. Wide: the right-hand column
           beside the artwork. */
        const float stack_l = x + 30.f * s;
        const float stack_r = x + w - 30.f * s;
        const float stack_t = y + player_h;
        const float stack_b = y + h - 12.f * s;

        const float wide_l  = thumb_x + thumb_size + 30.f * s;
        const float wide_r  = x + w - 26.f * s;
        const float wide_t  = y + 34.f * s;
        const float wide_b  = y + h - 30.f * s;

        const float col_l = stack_l * (1.f - wa) + wide_l * wa;
        const float col_r = stack_r * (1.f - wa) + wide_r * wa;
        const float top   = stack_t * (1.f - wa) + wide_t * wa;
        const float bot   = stack_b * (1.f - wa) + wide_b * wa;

        const float pad_x  = col_l - x;
        const float line_h = 26.f * s;

        /* @drawn at the size it was baked at.

           get() hands back the smallest bake at or above what is asked for, so
           asking for 13.5 got the 16px bake scaled down -- which is exactly the
           softness that reads as "pixelated". Taking the bake's own size draws
           the glyphs one texel to one pixel. */
        float size = 18.f * s;
        ImFont* lf = reg_pool.get(size);
        if (!lf) lf = bold_pool.get(size);
        const ImTextureID ltex = (lf == bold_pool.get(size))
            ? bold_pool.texture_id() : reg_pool.texture_id();
        if (lf && lf->FontSize > 0.f && lf->FontSize < size * 1.6f)
            size = lf->FontSize;

        dl->PushClipRect(ImVec2(col_l, top), ImVec2(col_r, bot), true);

        if (!lf) {
            /* nothing to draw with */
        } else if (!doc || doc->empty()) {
            const char* msg = doc ? "This song has no lyrics" : "Looking for lyrics...";
            const float message_width = std::max(40.f, col_r - col_l);
            const ImVec2 sz = lf->CalcTextSizeA(size, FLT_MAX, message_width, msg);
            dl->PushTextureID(ltex);
            dl->AddText(lf, size,
                ImVec2((col_l + col_r) * 0.5f - sz.x * 0.5f,
                       top + (bot - top) * 0.5f - sz.y * 0.5f),
                IM_COL32(255, 255, 255, static_cast<int>(180.f * lyr_present)), msg, nullptr, message_width);
            dl->PopTextureID();
        } else {
            // Highlight at the LRC timestamp, using the extrapolated playback clock.
            const int cur = doc->synced ? doc->index_at(g_eff_pos_ms) : 0;

            /* @wrapped, and measured once per shape change.

               A long line used to run straight out of the column and get
               clipped mid-word. Wrapping means a line no longer occupies one
               row, so the scroll can no longer be `index * line_h` -- it needs
               the running height of everything above it. Measuring every line
               every frame would be hundreds of CalcTextSize calls, so the
               layout is cached and rebuilt only when the doc, the column width
               or the size actually change. */
            struct wrap_cache {
                std::shared_ptr<const lyrics_doc> doc;
                float             width = 0.f;
                float             size = 0.f;
                std::vector<float> y;       /* top of each line */
                std::vector<int>   rows;    /* how many rows it wraps to */
                std::vector<float> emphasis;
                float              total = 0.f;
            };
            static wrap_cache wc;

            const float col_w = std::max(40.f, col_r - col_l);

            if (wc.doc != doc || wc.width != col_w || wc.size != size) {
                wc.doc = doc;
                wc.width = col_w;
                wc.size = size;
                wc.y.assign(doc->lines.size(), 0.f);
                wc.rows.assign(doc->lines.size(), 1);
                wc.emphasis.assign(doc->lines.size(), 0.f);

                float run = 0.f;
                for (std::size_t i = 0; i < doc->lines.size(); ++i) {
                    const std::string_view sv = doc->text(i);
                    wc.y[i] = run;

                    if (sv.empty()) {
                        /* @a blank line is a beat, not a whole row.

                           LRC marks instrumental gaps with empty lines, and
                           giving them a full row left holes big enough to look
                           like the view had lost its place. */
                        wc.rows[i] = 0;
                        run += line_h;
                        continue;
                    }

                    /* @measured with the WIDER cut, always.

                       The current line is drawn in bold, which wraps a row
                       further than the regular one it was measured with -- so a
                       line that budgeted one row drew two and landed on top of
                       its neighbour. Measuring everything against bold costs a
                       little slack on the dim lines and makes an overlap
                       impossible. */
                    ImFont* mf = bold_pool.get(size);
                    if (!mf) mf = lf;

                    const ImVec2 m = mf->CalcTextSizeA(size, FLT_MAX, col_w,
                                                       sv.data(), sv.data() + sv.size());
                    const int rows = std::max(1,
                        static_cast<int>(std::ceil(m.y / std::max(1.f, size) - 0.01f)));

                    wc.rows[i] = rows;
                    run += line_h * static_cast<float>(rows);
                }
                wc.total = run;
            }

            /* @the current line is pinned, the rest scroll past it.

               Eased rather than snapped: the jump between two lines is a whole
               row, and a view that teleports on every line is harder to read
               than one that slides. */
            static float s_scroll = 0.f;
            static float scroll_velocity = 0.f;
            static bool manual = false;
            static std::string browse_key;
            if (browse_key != doc->key
                || (!dt::vgui::bridge::isOverlayVisible() && !chat_open())) {
                manual = false;
                if (browse_key != doc->key) {
                    s_scroll = 0.f;
                    scroll_velocity = 0.f;
                }
                browse_key = doc->key;
            }
            const float sync_y = bot - 30.f * s;
            const bool over_lyrics = live && in_rect(mx, my, col_l, top,
                col_r - col_l, std::max(0.f, sync_y - top));
            if (over_lyrics && ImGui::GetIO().MouseWheel != 0.f) {
                manual = true;
                s_scroll = std::clamp(s_scroll - ImGui::GetIO().MouseWheel * line_h * 2.f,
                    0.f, std::max(0.f, wc.total - line_h));
            }
            const float want = (cur >= 0 && cur < static_cast<int>(wc.y.size()))
                             ? wc.y[static_cast<std::size_t>(cur)] : 0.f;
            const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.1f);
            if (!manual) {
                // Critically damped motion: gentle acceleration and settling,
                // independent of frame rate. Highlight timing stays on the song clock.
                constexpr float omega = 14.f;
                const float offset = s_scroll - want;
                const float impulse = scroll_velocity + omega * offset;
                const float decay = std::exp(-omega * dt);
                const float next_offset = (offset + impulse * dt) * decay;
                scroll_velocity = (scroll_velocity - omega * impulse * dt) * decay;
                s_scroll = want + next_offset;
                if (offset * next_offset < 0.f
                    || (std::abs(next_offset) < 0.05f && std::abs(scroll_velocity) < 0.5f)) {
                    s_scroll = want;
                    scroll_velocity = 0.f;
                }
            } else {
                scroll_velocity = 0.f;
            }

            // Leave room for the previous lyrics to recede above the active line.
            const float anchor = top + std::min(36.f * s, (bot - top) * 0.18f);

            /* @the intro, and every instrumental gap.

               Before the first line there is nothing to highlight, and a view
               that sits blank reads as broken rather than as waiting. Three
               dots on a travelling wave say "it is running, the words have not
               started" -- the same thing a typing indicator says, for the same
               reason. Also shown when the current line is empty, which is how
               LRC writes a break between verses. */
            const bool waiting = doc->synced
                && (cur < 0 || (cur < static_cast<int>(doc->lines.size())
                                && doc->text(static_cast<std::size_t>(cur)).empty()));

            /* @before the first line the dots need a row of their own.

               With nothing sung yet the scroll is zero, so line zero sits
               exactly on the anchor -- which is where the dots go, and they
               were being drawn straight through its text. Pushing the list down
               one row while waiting gives them somewhere to be. */
            const float pre = (cur < 0) ? line_h : 0.f;

            if (waiting) {
                const float now = static_cast<float>(ImGui::GetTime());
                const float dcy = anchor + (cur < 0 ? 0.f : wc.y[static_cast<std::size_t>(cur)])
                                - s_scroll + size * 0.45f;
                const float dr  = 3.2f * s;
                const float gapd = 11.f * s;

                for (int d = 0; d < 3; ++d) {
                    /* Each dot trails the one before it, so the group reads as
                       one wave passing through rather than three blinks. */
                    const float ph = now * 2.1f - static_cast<float>(d) * 0.55f;
                    const float k  = 0.5f + 0.5f * std::sin(ph);
                    const float rise = k * 3.4f * s;

                    dl->AddCircleFilled(
                        ImVec2(col_l + dr + static_cast<float>(d) * gapd, dcy - rise),
                        dr,
                        IM_COL32(255, 255, 255,
                                 static_cast<int>((70.f + 120.f * k) * lyr_present)),
                        12);
                }
            }

            dl->PushClipRect(ImVec2(col_l, top),
                ImVec2(col_r, manual && live ? sync_y - 5.f * s : bot), true);
            dl->PushTextureID(ltex);

            for (std::size_t i = 0; i < doc->lines.size(); ++i) {
                const bool is_cur = doc->synced && static_cast<int>(i) == cur;
                wc.emphasis[i] += ((is_cur ? 1.f : 0.f) - wc.emphasis[i])
                    * (1.f - std::exp(-10.f * dt));
                const float ly = anchor + pre + wc.y[i] - s_scroll;
                const float lh = line_h * static_cast<float>(std::max(1, wc.rows[i]));
                if (ly < top - lh || ly > bot) continue;

                const std::string_view sv = doc->text(i);
                if (sv.empty()) continue;

                /* The line being sung is white and the rest recede — the same
                   way every player does it, because it is what lets you find
                   your place without reading. */
                const bool hot_line = over_lyrics && doc->synced && track.can_seek
                    && track.duration_ms > 0 && my >= ly && my < ly + lh
                    && (!manual || my < sync_y - 5.f * s);
                if (hot_line) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        const long long seek_ms = std::clamp(doc->lines[i].t_ms, 0LL,
                            std::max(0LL, track.duration_ms));
                        push_command(cmd::seek, seek_ms);
                        s_display_ms = static_cast<double>(seek_ms);
                        g_eff_pos_ms = seek_ms;
                        manual = true;
                    }
                }
                // Position-based opacity follows the smooth scrolling motion.
                const float distance = std::abs(ly - anchor) / std::max(line_h, 1.f);
                const float ambient = doc->synced
                    ? 145.f * std::exp(-0.24f * distance) : 180.f;
                const int a = static_cast<int>((hot_line ? 255.f
                    : ambient + (255.f - ambient) * wc.emphasis[i]) * lyr_present);
                if (a <= 2) continue;

                /* The line being sung is set in the bold cut as well as full
                   white — weight is what the eye finds first, and colour alone
                   is not enough once the neighbours are also light. */
                ImFont* f_line = lf;
                ImTextureID f_tex = ltex;
                float f_size = size;

                if (is_cur) {
                    if (ImFont* bf = bold_pool.get(size)) {
                        f_line = bf;
                        f_tex  = bold_pool.texture_id();
                        // Keep the exact size used by the wrap measurement.
                        f_size = size;
                    }
                }

                dl->PopTextureID();
                dl->PushTextureID(f_tex);

                const int vertex_start = dl->VtxBuffer.Size;
                dl->AddText(f_line, f_size, ImVec2(col_l, ly),
                            IM_COL32(255, 255, 255, a),
                            sv.data(), sv.data() + sv.size(),
                            /*wrap_width=*/col_w);
                // LRC supplies phrase timestamps, so estimate the sung portion
                // between this phrase and the next distinct timestamp.
                if (is_cur && wa > 0.001f) {
                    const long long start_ms = doc->lines[i].t_ms;
                    long long end_ms = track.duration_ms;
                    for (std::size_t next = i + 1; next < doc->lines.size(); ++next) {
                        if (doc->lines[next].t_ms > start_ms) {
                            end_ms = doc->lines[next].t_ms;
                            break;
                        }
                    }
                    if (end_ms > start_ms) {
                        const float progress = static_cast<float>(std::clamp(
                            static_cast<double>(g_eff_pos_ms - start_ms)
                                / static_cast<double>(end_ms - start_ms), 0.0, 1.0));
                        // ImGui emits glyph quads in reading order, including
                        // wrapped rows. Accumulate their widths for one sweep.
                        float total_width = 0.f;
                        for (int v = vertex_start; v + 3 < dl->VtxBuffer.Size; v += 4)
                            total_width += std::max(0.f,
                                dl->VtxBuffer[v + 1].pos.x - dl->VtxBuffer[v].pos.x);
                        const float feather = std::max(1.f, f_size * 0.7f);
                        const float front = progress * (total_width + 2.f * feather) - feather;
                        float offset = 0.f;
                        for (int v = vertex_start; v + 3 < dl->VtxBuffer.Size; v += 4) {
                            const float left = dl->VtxBuffer[v].pos.x;
                            const float glyph_width = std::max(0.f,
                                dl->VtxBuffer[v + 1].pos.x - left);
                            for (int corner = 0; corner < 4; ++corner) {
                                ImDrawVert& vertex = dl->VtxBuffer[v + corner];
                                const float position = offset + vertex.pos.x - left;
                                float lit = std::clamp(
                                    (front - position) / feather + 0.5f, 0.f, 1.f);
                                lit = lit * lit * (3.f - 2.f * lit);
                                const float brightness = 1.f - wa * (1.f - lit) * 0.65f;
                                const ImU32 alpha = static_cast<ImU32>(a * brightness);
                                vertex.col = (vertex.col & ~IM_COL32_A_MASK)
                                    | (alpha << IM_COL32_A_SHIFT);
                            }
                            offset += glyph_width;
                        }
                    }
                }
                // Fade each glyph into the panel edges, including wrapped lines.
                const float fade_bottom = manual && live ? sync_y - 5.f * s : bot;
                const float fade_height = std::max(1.f,
                    std::min(line_h * 2.2f, (fade_bottom - top) * 0.28f));
                for (int v = vertex_start; v < dl->VtxBuffer.Size; ++v) {
                    ImDrawVert& vertex = dl->VtxBuffer[v];
                    float fade = std::clamp(std::min(vertex.pos.y - top,
                        fade_bottom - vertex.pos.y) / fade_height, 0.f, 1.f);
                    fade = fade * fade * (3.f - 2.f * fade);
                    const ImU32 alpha = static_cast<ImU32>(
                        ((vertex.col >> IM_COL32_A_SHIFT) & 0xff) * fade);
                    vertex.col = (vertex.col & ~IM_COL32_A_MASK)
                        | (alpha << IM_COL32_A_SHIFT);
                }
                dl->PopTextureID();
                dl->PushTextureID(ltex);
            }

            dl->PopTextureID();
            dl->PopClipRect();
            if (manual && live) {
                const float bw = 70.f * s;
                const float bx = (col_l + col_r - bw) * 0.5f;
                const bool hot = in_rect(mx, my, bx, sync_y, bw, 28.f * s);
                dl->AddRectFilled(ImVec2(bx, sync_y), ImVec2(bx + bw, sync_y + 28.f * s),
                    hot ? IM_COL32(65, 65, 72, 255) : IM_COL32(28, 28, 34, 245), 14.f * s);
                const ImVec2 label = lf->CalcTextSizeA(size, FLT_MAX, 0.f, "Sync");
                dl->PushTextureID(ltex);
                dl->AddText(lf, size, ImVec2(bx + (bw - label.x) * 0.5f,
                    sync_y + (28.f * s - label.y) * 0.5f), IM_COL32(255,255,255,255), "Sync");
                dl->PopTextureID();
                if (hot) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) manual = false;
                }
            }
        }

        dl->PopClipRect();
    }

    /* No hairline outline — the al_glass shadow already carries the card
       silhouette, and a stroke on top of a rounded rect adds visible
       corner artifacts (the "picos" the user reported). */

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

}
