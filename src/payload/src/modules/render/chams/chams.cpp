// chams.cpp — engine orchestration. See chams.h.

#include "chams.h"
#include "chams_debug.h"
#include "chams_crash.h"
#include "chams_geom.h"
#include "chams_cull.h"
#include "chams_item.h"
#include "chams_gl.h"
#include "item_shader_gl.h"   // ItemShader's own layer: wanted() for the arm gate
#include "chams_jni.h"

#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "core/UnloadSafety.h"
#include "vgui/bridge.h"     // isOverlayVisible() — hide the chams behind the panel

#include "imgui.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <chrono>

namespace hf  = dt::modules::hitfx;
namespace cap = dt::modules::hitfx::capture;

namespace dt::modules::chams {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

std::vector<jni::player_sample> g_players;
bool g_logged_banner = false;

// out = a * b, both column-major 4x4.
void mat_mul(const float* a, const float* b, float* out)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
}

// Build this module's camera from the shared capture service's frame.
//
// NOTE, recorded rather than hidden: the PlayerESP additionally folds in MC's
// view-bob matrix before the projection. This module does not yet, so with
// "View Bobbing" ON the fill can sit a pixel or two off the drawn model while
// walking. It is a known, bounded inaccuracy — not a silent one; the debug log
// records whether bob correction was applied so the effect can be attributed
// rather than guessed at.
bool build_camera(const hf::render_frame& f, geom::camera& out)
{
    if (!f.valid || f.viewport_w <= 0 || f.viewport_h <= 0) return false;
    mat_mul(f.projection, f.modelview, out.mvp);
    out.cam_x = f.cam_x; out.cam_y = f.cam_y; out.cam_z = f.cam_z;
    out.vw = static_cast<float>(f.viewport_w);
    out.vh = static_cast<float>(f.viewport_h);
    out.valid = true;
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
void install()
{
    // FIRST. Before anything that could fault, so a crash during bring-up is
    // reported rather than swallowed as a log that simply stops.
    crash::install();

    // The matrix-capture service is shared with hitfx and the ESP; installing
    // is idempotent, so whichever module gets there first pays for it.
    cap::install();

    ::dt::desklog::section(dbg::ch(), "chams install");
    CHAMS_LOG("install(): capture service installed = %d", (int)cap::is_installed());
    CHAMS_LOG("log file: %s", ::dt::desklog::path_of(dbg::ch()));
    VLOG_INFO("chams", "install() done");
}

void shutdown()
{
    gl::set_active(false);
    if (!gl::uninstall()) {
        ::dt::blockUnload();
        VLOG_ERR("chams", "Hooks did not drain; keeping callbacks and caches alive");
        return;
    }
    cull::shutdown();
    item::shutdown();
    g_players.clear();
    ::dt::jvm::JniScope s;
    if (s) jni::release_cache(s.get());
    CHAMS_LOG("shutdown()");
    crash::shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
void tick_game_context()
{
    // Arm once. install() itself early-outs when already installed and when no
    // context is current yet, so this is a cheap atomic read in the steady
    // state. Retried every frame until it takes, because the first frames of a
    // Sodium/Iris startup are exactly when it can fail.
    if (!gl::installed()) {
        static int s_attempts = 0;
        if (s_attempts < 600) {          // ~10 s at 60 fps, then stop trying
            ++s_attempts;
            if (gl::install())
                CHAMS_LOG("gl layer armed after %d frame(s)", s_attempts);
            else if (s_attempts == 600)
                CHAMS_LOG("gl layer FAILED to arm after %d frames — staying inert", s_attempts);
        }
        return;
    }

    // Roll the per-frame counters. This is the one guaranteed once-per-frame
    // point on the game's context.
    CHAMS_MARK("tick_game_context -> gl::frame_boundary");
    gl::frame_boundary();

    // The gate is the module's enabled state. Never install/remove per toggle:
    // MinHook's enable/disable suspends every thread in the process, and doing
    // that mid-frame from a ClickGui click is how you deadlock a render thread.
    //
    // The ItemShader module (first-person held-item shader) also needs the layer
    // armed, so it is ORed in — the layer runs when EITHER feature wants it, and
    // each keys off its own gate inside the detour.
    const bool want_layer = g_enabled || ::dt::modules::itemshader::wanted();
    gl::set_active(want_layer);

    // Classify any newly-bound program by reading its GLSL back. Runs here
    // because this is a frame boundary on the GAME's context — never from a
    // draw detour. ItemShader wants this too so its FP-item program is
    // identified as an entity program in the log.
    if (want_layer) {
        CHAMS_MARK("tick_game_context -> gl::classify_pending_programs");
        gl::classify_pending_programs();
    }

    // BOTH modes are the native path now. Minecraft draws the player, we replay
    // that draw with the depth test defeated; the only difference is whether a
    // white texture and constant-colour blending are swapped in first.
    //
    // The ImGui overlay is gone from the picture entirely. It could never draw
    // the real skin, its fill read as a glitch beside real players, and its
    // whole reason for existing — covering players Minecraft refused to draw —
    // disappeared once EntityCulling was defeated properly.
    item::on_frame();

    CHAMS_MARK("tick_game_context -> setters");
    gl::set_replay(g_enabled);
    gl::set_skip_blended(false);
    gl::set_flat_colour(false, 0.f, 0.f, 0.f, 0.f);   // colour mode retired
    gl::set_diagnostics(false);                        // per-draw locks off
}

// ─────────────────────────────────────────────────────────────────────────────
void render_text(int display_w, int display_h)
{
    CHAMS_MARK("render_text -> enter");
    if (g_settings_provider) g_settings_provider(g_settings);

    if (!g_enabled || display_w < 2 || display_h < 2) {
        if (!g_players.empty()) g_players.clear();
        return;
    }

    // Do not paint the chams over the ClickGui.
    if (::dt::vgui::bridge::isOverlayVisible()) {
        g_players.clear();
        return;
    }

    const bool native_mode = true;   // the only path there is

    if (!gl::installed()) {
        // Distinguish "the feature is off" from "the feature is broken". Only
        // the log can tell those apart, and silence would look identical.
        CHAMS_RL(dbg::kSlotFallbackNormal, 2000,
                 "mode=Normal but the GL layer is NOT armed — nothing will draw. "
                 "Check the 'GL hook installation' section above.");
        g_players.clear();
        return;
    }

    // Retry the capture install until a world exists.
    if (!cap::is_installed()) {
        static double s_next = 0.0;
        const double now = ImGui::GetTime();
        if (now >= s_next) { s_next = now + 1.0; cap::install(); }
        CHAMS_RL(dbg::kSlotNoCamera, 2000, "capture service not installed yet");
        return;
    }

    // POLL THROTTLE — and note carefully what it does NOT cover.
    //
    // Nothing the player sees comes from this poll. The chams itself is the GL
    // replay, which runs on every draw call the game makes, at full framerate,
    // using Minecraft's own geometry and its own interpolation. Smoothness is
    // completely independent of how often this runs.
    //
    // The poll only does bookkeeping: it marks players not-culled — and each
    // such call buys 1000 ms of cover, so 50 ms leaves a 20x margin and nobody
    // can blink out — and it publishes which textures count as a player, which
    // changes only when someone joins, leaves or swaps a skin.
    //
    // What it cost per frame was real: ~25 JNI calls per player, so at 20
    // players and 200 fps roughly 100k JNI calls a second on the render thread,
    // to recompute an answer that had not changed.
    //
    // The throttle is placed AROUND THE POLL ONLY. An earlier arrangement put
    // the whole block behind it, which would have dropped the shared
    // matrix-capture service to 20 Hz as well — and the PlayerESP and hitfx
    // both read their camera from that service, so their overlays really would
    // have gone choppy. Capture stays per-frame below.
    static double s_next_poll = 0.0;
    const bool do_poll = (ImGui::GetTime() >= s_next_poll);
    if (do_poll) s_next_poll = ImGui::GetTime() + 0.05;

    geom::camera cam{};
    {
        ::dt::jvm::JniScope s;
        if (!s) { CHAMS_RL(dbg::kSlotNoWorld, 2000, "no JNIEnv on the render thread"); return; }
        JNIEnv* env = s.get();

        if (!g_logged_banner) {
            g_logged_banner = true;
            // A GL context IS current here: gl_hook made its own current before
            // calling into the foreground pass.
            ::dt::desklog::banner(dbg::ch(), /*have_gl_context=*/true);
        }

        // ARM THE CAPTURE SERVICE FIRST. capture_now() opens with
        //     if (!g_on.load(...)) return;
        // (hitfx_capture_hook.cpp:194) and g_on is only ever raised by
        // set_capture_enabled. Calling capture_now without this is a silent
        // no-op: the service stays installed and healthy, publishes nothing,
        // and latest_frame keeps handing back valid=0 forever. That is exactly
        // how this module failed on its first run — the JNI cache resolved
        // 36/37 and the log then repeated "no captured camera frame yet"
        // indefinitely. It only looked like a capture bug because the ESP
        // happens to arm the same shared service, so enabling the ESP masked it.
        CHAMS_MARK("poll -> cap::capture_now");
        cap::set_capture_enabled(true);
        cap::capture_now(env);

        CHAMS_MARK("poll -> jni::ensure_cache");
        if (!jni::ensure_cache(env)) return;

        // Defeat the game's own entity culling for players. Retried until it
        // takes: the dispatcher class does not exist before a world is loaded.
        // Gated on the module being on AND the native path being selected —
        // forcing extra entity rendering is only worth paying for when
        // something is going to consume it.
        if (!cull::installed()) {
            static int s_tries = 0;
            if (s_tries < 600) {
                ++s_tries;
                // JNIHook rewrites a live Java method. If anything in this
                // module is going to take the process down, it is most likely
                // this line, so the breadcrumb has to be set BEFORE the call.
                CHAMS_MARK("poll -> cull::install (JNIHook_Attach shouldRender)");
                cull::install(env);
                CHAMS_MARK("poll -> cull::install returned");
            }
        }
        cull::set_active(g_enabled && native_mode);

        // Bracket a player's held item so its draw can be recognised. Same
        // retry shape as the cull hook: the feature-renderer class does not
        // exist before a world is loaded.
        if (!item::installed()) {
            static int s_item_tries = 0;
            if (s_item_tries < 600) {
                ++s_item_tries;
                CHAMS_MARK("poll -> item::install (JNIHook_Attach heldItem render)");
                item::install(env);
                CHAMS_MARK("poll -> item::install returned");
            }
        }
        item::set_active(g_enabled && native_mode);
        item::log_summary();

        // Defeat tr7zw's EntityCulling for polled players. Needed in BOTH modes:
        // the native path needs the game to issue the draw at all, and the
        // overlay path is unaffected either way, so leaving it on with the
        // module is the simplest correct rule.
        CHAMS_MARK("poll -> set_defeat_entity_culling");
        jni::set_defeat_entity_culling(g_enabled);

        hf::render_frame f{};
        if (!hf::latest_frame(f) || !build_camera(f, cam)) {
            // Report the fire count too: it separates "the service never ran"
            // from "it ran and produced nothing usable", which are different
            // bugs in different files.
            CHAMS_RL(dbg::kSlotNoCamera, 2000,
                     "no captured camera frame yet (valid=%d vp=%dx%d fires=%llu installed=%d)",
                     (int)f.valid, f.viewport_w, f.viewport_h,
                     cap::debug_fire_count(), (int)cap::is_installed());
            return;
        }

        CHAMS_RL(dbg::kSlotFrame, 2000,
                 "frame: vp=%.0fx%.0f fb=%dx%d cam=(%.2f,%.2f,%.2f) capture_frame=%llu",
                 cam.vw, cam.vh, display_w, display_h,
                 cam.cam_x, cam.cam_y, cam.cam_z, f.frame);

        // THE THROTTLED PART, and only this. Capture above and the culling arm
        // just before stay per-frame.
        if (do_poll) {
            jni::poll_players(env, cam.cam_x, cam.cam_y, cam.cam_z,
                              g_settings.range, g_settings.showself,
                              g_settings.max_targets, /*want_visibility=*/false,
                              /*want_skin=*/true, g_players);
        }
    }

    // ── Per-player narrowing ────────────────────────────────────────────────
    //
    // Publish the GL texture names of the skins belonging to the players we just
    // polled. The native layer then replays only draws whose bound texture is
    // one of these, instead of every entity draw — which is what has been
    // putting the effect on mobs, item frames, dropped items and the player's
    // own first-person hand.
    //
    // Published even in Colour mode: it costs nothing, and it means switching
    // modes does not need a frame to warm up.
    {
        if (!do_poll) { /* nothing changed since the last poll */ } else {
        static std::vector<unsigned int> s_skins;
        s_skins.clear();
        for (const jni::player_sample& p : g_players)
            if (p.skin_gl) s_skins.push_back(p.skin_gl);
        // ARMOUR joins the skins. A player is their skin AND whatever they are
        // wearing: replaying only the skin means the body sorts over its own
        // armour in the open (the flat white figure that was reported) and the
        // copy seen through a wall is a bare body.
        //
        // Resolved once and cached -- these are shared assets whose GL names
        // only change on a resource reload, so paying for ~44 JNI lookups every
        // poll would be waste. A reload can renumber them, so an empty result
        // is retried rather than remembered as final.
        // Re-resolved on a timer, NOT cached for the session. See the note
        // above the counters in chams_gl.cpp for the measurement that forced
        // this: armour draws flatlined a few seconds in while player draws held
        // their rate, and skins differ from armour in exactly one respect --
        // skins are re-resolved every poll and armour was not.
        //
        // Two seconds is a long time next to a frame and nothing next to a
        // texture reload, and the work is ~41 JNI calls, so the cost does not
        // need defending.
        static std::vector<unsigned int> s_armour;
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point s_armour_at{};
            const auto now = clock::now();
            if (s_armour.empty() || now - s_armour_at > std::chrono::seconds(2)) {
                ::dt::jvm::JniScope js;
                if (js) {
                    unsigned int tmp[64];
                    const int n = jni::equipment_textures(js.get(), tmp, 64);
                    std::vector<unsigned int> fresh(tmp, tmp + (n < 0 ? 0 : n));

                    // Say so when a name rotates. If this line never appears the
                    // theory above is wrong and I want to know that rather than
                    // keep believing it; if it appears repeatedly, a one-shot
                    // resolution was never going to work on this setup.
                    if (!s_armour.empty() && fresh != s_armour) {
                        int changed = 0;
                        const size_t m = fresh.size() < s_armour.size()
                                       ? fresh.size() : s_armour.size();
                        for (size_t i = 0; i < m; ++i)
                            if (fresh[i] != s_armour[i]) ++changed;
                        CHAMS_LOG("armour texture ids ROTATED: %d of %d changed "
                                  "(was %u.. now %u..) -- a one-shot resolution "
                                  "would have gone stale here",
                                  changed, (int)m,
                                  s_armour.empty() ? 0u : s_armour[0],
                                  fresh.empty() ? 0u : fresh[0]);
                    }
                    if (!fresh.empty() || s_armour.empty()) s_armour.swap(fresh);
                    s_armour_at = now;
                }
            }
        }

        // ── WHAT GOES INTO THE PLAYER SET, AND WHAT MUST NOT ────────────
        //
        // Armour textures are SHARED assets: one GL object per material for
        // every wearer in the world. Folding them into the player set makes
        // classify() answer "player" for anyone's iron chestplate -- an
        // armoured zombie's, an armour stand's, and above all your own. That is
        // a route to self-chams that showself cannot close, because showself
        // only decides which SKINS get published.
        //
        // Once the armour queue is armed the ownership signal for armour is
        // exact (the render state's entity id), so the crutch is dropped. If
        // either half of that path failed to attach, armour_queue_armed() is
        // false and the union stays -- partial coverage beats none.
        // Default skins, resolved on the same timer as armour and for the same
        // reason: they are shared assets with no per-player accessor.
        static std::vector<unsigned int> s_defskin;
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point s_def_at{};
            const auto now = clock::now();
            if (s_defskin.empty() || now - s_def_at > std::chrono::seconds(5)) {
                ::dt::jvm::JniScope js2;
                if (js2) {
                    unsigned int tmp[32];
                    const int n = jni::default_skin_textures(js2.get(), tmp, 32);
                    if (n > 0) s_defskin.assign(tmp, tmp + n);
                    s_def_at = now;
                }
            }
        }

        std::vector<unsigned int> publish = s_skins;
        publish.insert(publish.end(), s_defskin.begin(), s_defskin.end());
        if (!item::armour_queue_armed())
            publish.insert(publish.end(), s_armour.begin(), s_armour.end());

        // And never publish our own skin, even when a remote player resolves to
        // the very same texture object -- two accounts on the identical default
        // skin share one GL name. When that happens the id can serve one owner
        // or neither, and "not me" is the side to err on.
        const unsigned int self_tex = jni::self_skin_gl();
        if (self_tex) {
            publish.erase(std::remove(publish.begin(), publish.end(), self_tex),
                          publish.end());
        }
        // Handed over separately: our own draws get a depth-only pass into the
        // band rather than a chams, so other players stop showing through us.
        gl::publish_self_texture(self_tex);

        gl::publish_player_textures(publish.empty() ? nullptr : publish.data(),
                                    static_cast<int>(publish.size()));
        gl::publish_armour_textures(s_armour.empty() ? nullptr : s_armour.data(),
                                    static_cast<int>(s_armour.size()));
        CHAMS_RL(dbg::kSlotLiveModel, 3000,
                 "player skins resolved: %d of %d players (+%d armour textures)",
                 (int)s_skins.size(), (int)g_players.size(), (int)s_armour.size());

        // ── PUT A NAME ON WHAT THE GAME ACTUALLY BINDS ──────────────────────
        //
        // Armour recognition is an identity match against ids resolved from the
        // vanilla asset paths, and every failure of it looks the same from the
        // inside: the draw is classified Entity, nothing is replayed, and the
        // armour is simply absent. Which is also exactly what "the armour is
        // never drawn" looks like. No counter we own separates the two.
        //
        // So ask the other direction once, out loud: here are the ids an entity
        // draw bound that we could not account for -- what are they called? The
        // dump answers with identifiers, and an identifier is a fact rather than
        // a suspicion. If equipment is being rebound to something outside the
        // vanilla paths, its name is in that list.
        //
        // Runs at most once every twelve seconds and only while diagnostics are
        // on, because it walks the whole texture registry.
        {
            // Deliberately NOT immediate. The first run of this fired 15 ms
            // after activation and reported known=0 unknown=0, because no
            // equipment had been drawn yet -- an empty answer to the one
            // question worth asking. It now waits until the brackets have had
            // real work to do, and runs a bounded number of times rather than
            // forever, because walking the texture map is not free and the
            // answer does not change once it is known.
            using clock = std::chrono::steady_clock;
            static clock::time_point s_first{};
            static clock::time_point s_last_dump{};
            static int s_dumps = 0;
            const auto now = clock::now();
            if (s_first.time_since_epoch().count() == 0) s_first = now;
            if (s_dumps < 4
                && now - s_first  > std::chrono::seconds(12)
                && now - s_last_dump > std::chrono::seconds(15)) {
                ++s_dumps;
                // The busiest textures first: this is the table that answers
                // the armour question outright.
                std::uint32_t tids[24], tcnt[24];
                unsigned char tfl[24];
                const int nt = gl::top_textures(tids, tcnt, tfl, 24);
                for (int i = 0; i < nt; ++i) {
                    const char* what = (tfl[i] & 4) ? "SELF"
                                     : (tfl[i] & 2) ? "armour"
                                     : (tfl[i] & 1) ? "player"
                                                    : "UNKNOWN";
                    CHAMS_LOG("toptex: gl=%-5u draws=%-9u %s", tids[i], tcnt[i], what);
                }

                std::uint32_t unknown[24], equip[24];
                unsigned long long ek = 0, eu = 0;
                const int nu = gl::unaccounted_textures(unknown, 24);
                const int ne = gl::equipment_unaccounted(equip, 24, ek, eu);

                char ids[256];
                int off = 0;
                for (int i = 0; i < ne && off < (int)sizeof(ids) - 12; ++i)
                    off += std::snprintf(ids + off, sizeof(ids) - off, "%u ", equip[i]);
                ids[off > 0 ? off - 1 : 0] = '\0';

                // This line is the verdict, and it reads without a decoder ring:
                //   equipUnknown near zero  -> equipment textures ARE the ones we
                //                              resolved; the armour fault is
                //                              downstream of identification.
                //   equipUnknown dominant   -> equipment is being drawn with
                //                              textures the vanilla asset paths
                //                              do not name.
                CHAMS_LOG("texdump: equipment draws: known=%llu unknown=%llu | "
                          "%d distinct unknown equipment ids [%s] | "
                          "%d unaccounted entity ids overall",
                          ek, eu, ne, ids, nu);

                if (nu > 0 || ne > 0) {
                    s_last_dump = now;
                    std::uint32_t ask[72];
                    int na = 0;
                    for (int i = 0; i < nt && na < 72; ++i) ask[na++] = tids[i];
                    for (int i = 0; i < ne && na < 72; ++i) ask[na++] = equip[i];
                    for (int i = 0; i < nu && na < 72; ++i) ask[na++] = unknown[i];
                    ::dt::jvm::JniScope ds;
                    if (ds) jni::dump_texture_registry(ds.get(), ask, na);
                }
            }
        }
        }
    }

    // ── The two paths cover each other ──────────────────────────────────────
    //
    // The replay in chams_gl.cpp is the whole feature; nothing is drawn here.
    // What remains below is a rate-limited health line, kept because it is the
    // one place that says whether the game is actually issuing player draws.
    CHAMS_RL(dbg::kSlotProject, 2000,
             "chams: %d players polled, %d of them drawn by the game last frame",
             (int)g_players.size(), gl::last_frame_replays());

    gl::log_frame_summary();
    cull::log_summary();

    ::dt::desklog::dump_counters_rl(dbg::ch(), 5000);
}

} // namespace dt::modules::chams

