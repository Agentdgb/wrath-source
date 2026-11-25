#pragma once

#include "lyrics.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace dt::modules::spotify {

extern bool g_enabled;
void set_chat_open(bool open);
bool chat_open();

struct settings {
    int background = 0; // Artwork, Ember, Silk
    float scale        = 1.0f;
    float rounding     = 1.0f;
    bool  show_progress = true;

    /* @the frosted pane behind the card.

       Deliberately NOT tied to the client-wide Glass toggle, which the rest
       of the HUD checks. That flag starts false on every launch and is never
       written to the profile, so ANDing with it would delete the look this
       card already ships with and hand the user a checkbox that does nothing
       until they find an unrelated switch elsewhere. This one is the card
       own setting and it persists. */
    bool  blur = true;
    float accent_r = 255.f/255.f, accent_g = 61.f/255.f,  accent_b = 148.f/255.f, accent_a = 1.f;

    /* Spotify Web API — Client Credentials flow. Both must be set for the
       worker to reach api.spotify.com; otherwise it falls back to Deezer
       (no auth) and then iTunes. Register a free app at
       https://developer.spotify.com/dashboard to get these. */
    std::string spotify_client_id;
    std::string spotify_client_secret;
};
extern settings                        g_settings;
extern std::mutex                      g_settings_mx;
settings                               get_settings_snapshot();

extern std::function<void(settings&)> g_settings_provider;
struct writers {
    std::function<void(const char*, bool)>  bool_;
    std::function<void(const char*, float)> float_;
    std::function<void(const char*, int)>   int_;
    std::function<void(const char*, float, float, float, float)> color;
};
extern writers g_writers;

struct track_info {
    std::string title;
    std::string artist;

    /* What the player says the track belongs to. Used to pick the right sleeve
       out of a catalogue search -- a title and an artist alone match every
       reissue, single and live cut of the same song. */
    std::string album;
    long long   position_ms   = 0;
    long long   duration_ms   = 0;

    /* Timeline start, in ms. SMTC reports position relative to this, and a
       seek has to be converted back to absolute before it is sent. The worker
       was already computing it and discarding it. */
    long long   start_ms      = 0;

    /* What the session says it will accept. Spotify does not always report a
       usable seek, and a slider that silently does nothing is worse than one
       that is not drawn. */
    bool        can_seek      = false;

    /* Whether the session takes a shuffle change, and whether it is currently
       on -- the button lights when it is. */
    bool        can_shuffle   = false;
    bool        shuffled      = false;
    /* Wall-clock timestamp (steady_clock, nanoseconds) at which the
       position_ms above was snapshotted. Render extrapolates forward
       from here so the progress bar moves smoothly between the SMTC
       polls (which fire only twice a second). */
    long long   snapshot_ns   = 0;
    bool        playing       = false;
    unsigned int thumb_tex_id      = 0;
    unsigned int blurred_bg_tex_id = 0;

    /* A gentler blur of the same cover, for the frosted band in cover mode.
       The one above is destroyed on purpose and is wrong as glass. */
    unsigned int band_tex_id      = 0;
    unsigned int ember_tex_id = 0, silk_tex_id = 0;
};
/* @what the card can ask the player to do.

   The render thread never touches SMTC. The session interfaces are file-local
   to the worker and its apartment was initialised on the worker's own thread,
   so a call from the frame would be crossing both. Clicks are queued here and
   the worker drains them at the top of its next poll, while it still holds a
   live session. */
enum class cmd : int { toggle_play, next, prev, seek, shuffle };

/* `arg_ms` is the absolute position for seek, ignored otherwise. Safe from the
   render thread. Silently dropped if the queue is already deep -- a click that
   arrives while Spotify is closed must not fire minutes later. */
void push_command(cmd c, long long arg_ms = 0);

/* Drained by the worker. Returns false when there is nothing to do. */
bool pop_command(cmd& out, long long& arg_ms);

/* Blocks the worker until a command lands or the timeout expires. Replaces the
   fixed sleep between polls, so a press is acted on at once. */
void wait_for_command(int timeout_ms);

/* Flips the local play state so the glyph responds on the frame of the click
   rather than after the round trip. Overwritten by the next poll. */
void assume_playing(bool playing);

/* @the lyrics for the current track, or null.

   A shared_ptr and not a member of track_info: the draw pass deep-copies the
   track every frame, and a few kilobytes of text does not belong in something
   copied sixty times a second. Swapping the pointer is the whole publish. */
std::shared_ptr<const lyrics_doc> get_lyrics();
void set_lyrics(std::shared_ptr<const lyrics_doc> doc);
void begin_lyrics(const std::string& key);

void        set_track(const track_info& t);
void        clear_track();
track_info  get_track();

/* Queue a raw RGBA image for upload on the render thread. The worker calls
   this with decoded album-art bytes; the render loop drains the queue,
   creates a GL texture, and stores the resulting id in *out_tex. `out_tex`
   is a raw atomic pointer that must outlive the upload (spotify_worker
   keeps static atomics for the current thumb / bg tex ids). */
#include <atomic>
#include <vector>
#include <cstdint>
void queue_texture_upload(std::vector<std::uint8_t> px, int w, int h,
                          std::atomic<unsigned int>* out_tex, std::function<bool()> still_current = {});

void render_background(int display_w, int display_h, unsigned int bound_fb);
void render_text(int display_w, int display_h);

void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);

}
