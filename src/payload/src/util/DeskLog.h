// DeskLog.h — advanced desktop debug logger for module bring-up.
//
// Why this exists
// ---------------
// src/modules/render/esp/esp_debug.cpp already does "write a log to the
// Desktop", but it is hardcoded to one machine's path
// ("C:\Users\vldwd\Desktop\vanta_esp.txt") and only falls back to
// %USERPROFILE%\Desktop, which is WRONG on any machine where OneDrive has
// redirected the Desktop folder — the file silently lands in %TEMP% and the
// developer concludes the module never ran. This resolves the real Desktop
// through SHGetKnownFolderPath(FOLDERID_Desktop), which follows the
// redirection, and only then falls back.
//
// It is also channel-based: each subsystem gets its own file, so bringing up
// Chams does not interleave with the ESP's own spam.
//
// Everything here is bring-up scaffolding. It is deliberately synchronous and
// flushes after every line: a log that loses its tail to a buffered crash is
// worse than no log, and the frame cost only matters while you are debugging.
//
// Threading: every entry point takes the channel's mutex. Safe to call from the
// wglSwapBuffers/render thread, the game pump, and JVMTI callbacks.

#pragma once

#include <cstdarg>
#include <cstdint>

namespace vanta::desklog {

// ─────────────────────────────────────────────────────────────────────────────
// Channel handle. Cheap to copy. Obtain one with open().
// ─────────────────────────────────────────────────────────────────────────────
struct Channel {
    int id = -1;
    bool valid() const { return id >= 0; }
};

// Opens (truncating) "<Desktop>\<basename>.txt" and returns a handle.
// Idempotent per basename: calling it twice with the same name returns the same
// channel and does NOT re-truncate. Never throws; on total failure the returned
// channel is still usable (every write becomes a no-op plus an
// OutputDebugStringA), so call sites need no error handling.
Channel open(const char* basename);

// Where the channel actually ended up, UTF-8, or "" if nothing opened. Useful to
// print into the game chat / main log so the developer can find the file.
const char* path_of(Channel ch);

// ─────────────────────────────────────────────────────────────────────────────
// Writing
// ─────────────────────────────────────────────────────────────────────────────

// printf-style. NOTE: this is printf semantics ("%d"), unlike VLOG_* in
// util/Log.h which uses "{}" placeholders. Mixing the two up produces a line
// with a literal %d in it, which is the usual first confusion.
void logf(Channel ch, const char* fmt, ...);
void vlogf(Channel ch, const char* fmt, va_list ap);

// Rate-limited: this call site prints at most once per `min_ms`. `slot` must be
// unique per call site within the channel (0..kSlots-1). Use for anything
// inside the per-frame or per-entity loop.
//
// PREFER logf_site BELOW. Hand-assigned slots are a footgun: two call sites
// given the same number silently suppress each other, and the symptom is a
// missing log line — the hardest thing to notice, because nothing is wrong on
// screen and nothing is wrong in the file, there is simply an absence. That
// failure happened three separate times while bringing up the Chams module, and
// each time it cost a full debugging round trip chasing the wrong hypothesis.
constexpr int kSlots = 128;
void logf_rl(Channel ch, int slot, unsigned min_ms, const char* fmt, ...);

// Rate-limited, keyed by CALL SITE rather than by a hand-assigned number.
//
// `site` must be a pointer that is unique and stable per call site — the
// address of a function-local static is exactly that, which is what the
// VDESK_RL_SITE macro below supplies. Two call sites can no longer collide,
// because the compiler hands out the identities instead of the programmer.
void logf_site(Channel ch, const void* site, unsigned min_ms, const char* fmt, ...);

// Prints exactly once for the lifetime of the process, then never again.
// `slot` is again a unique per-call-site id.
void logf_once(Channel ch, int slot, const char* fmt, ...);

// A visually separated block header, so a dump is findable by eye:
//   ──────── name ────────
void section(Channel ch, const char* name);

// Marks the start of a rendered frame and gives every subsequent line an
// "f<N>" prefix. Call once per frame from the render pass. Returns the frame
// number. Optional — lines are still written if you never call it.
unsigned long long frame_begin(Channel ch);

// ─────────────────────────────────────────────────────────────────────────────
// Structured dumps — the things you always end up hand-rolling
// ─────────────────────────────────────────────────────────────────────────────

// 4x4 matrix, column-major (org.joml / GL layout — the layout render_frame
// uses). Printed as 4 rows of 4 for readability.
void mat4(Channel ch, const char* label, const float m[16]);

void vec3f(Channel ch, const char* label, float x, float y, float z);
void vec3d(Channel ch, const char* label, double x, double y, double z);

// Hex + ASCII dump. Capped at 4 KiB per call.
void hexdump(Channel ch, const char* label, const void* data, unsigned long len);

// ─────────────────────────────────────────────────────────────────────────────
// Counters — for "how many times did this happen this second"
// ─────────────────────────────────────────────────────────────────────────────
// bump() is cheap and lock-free-ish; dump_counters() prints every non-zero
// counter and resets them. Typical use: bump on every draw call you classify,
// dump once a second.

void bump(Channel ch, const char* name, long long by = 1);
void dump_counters(Channel ch);
// Prints the counters at most once per `min_ms` (and resets). Call every frame.
void dump_counters_rl(Channel ch, unsigned min_ms);

// ─────────────────────────────────────────────────────────────────────────────
// OpenGL helpers
// ─────────────────────────────────────────────────────────────────────────────
// IMPORTANT: gl_check drains glGetError and logs every code. It must only be
// called on a thread with a current GL context. Sodium forces
// GL_DEBUG_OUTPUT_SYNCHRONOUS on NVIDIA, which turns a GL error into an
// in-driver abort that kills javaw — so leaving errors undrained is not a
// cosmetic problem. Returns the number of errors drained.
int gl_check(Channel ch, const char* where);

// Snapshots the fixed-function/state bits that matter for through-wall
// rendering (depth test/func/mask, blend, cull, viewport, scissor, bound
// framebuffer, bound program, bound texture) and writes them as one line.
// Requires a current GL context. Rate-limited by the caller.
void gl_state(Channel ch, const char* where);

// One-time environment banner: process, GL vendor/renderer/version, GLSL
// version, context version/profile mask, and the graphics-related NATIVE
// modules loaded into the process (which ICD is in play: nvoglv64 / atio6axx /
// igdgl...). Requires a current context for the GL strings; pass
// have_gl_context=false to skip those.
//
// It deliberately does NOT claim to detect Sodium or Iris: those are Java mods
// and load no native module of their own, so a native module scan cannot see
// them. Detecting them has to happen Java-side (a class lookup for
// net/caffeinemc/... or net/irisshaders/...), which is out of scope here.
void banner(Channel ch, bool have_gl_context);

// ─────────────────────────────────────────────────────────────────────────────
// JNI resolution reporting
// ─────────────────────────────────────────────────────────────────────────────
// The single most common bring-up failure in this codebase is a jclass/
// jmethodID/jfieldID that resolved to null on one MC version, after which the
// module silently does nothing. Report every lookup through here and the log
// tells you exactly which one died instead of leaving you to bisect.

void jni_lookup(Channel ch, const char* kind, const char* owner,
                const char* name, const char* desc, bool ok);

// Prints a summary table of every jni_lookup seen so far: total, resolved,
// and the full list of failures. Call once after your cache init.
void jni_summary(Channel ch);

// ─────────────────────────────────────────────────────────────────────────────
void flush(Channel ch);
void close_all();

} // namespace vanta::desklog

// Convenience macros. They take a Channel expression as the first argument, so
// a module can do:  #define CH ::vanta::modules::chams::dbg_channel()
#define VDESK_LOG(ch, ...)            ::vanta::desklog::logf((ch), __VA_ARGS__)
#define VDESK_RL(ch, slot, ms, ...)   ::vanta::desklog::logf_rl((ch), (slot), (ms), __VA_ARGS__)

// Collision-proof rate limiting. The function-local static gives this call site
// an address nothing else in the program shares, and that address is the key.
#define VDESK_RL_SITE(ch, ms, ...)                                        \
    do {                                                                  \
        static const char kDeskLogSite = 0;                               \
        ::vanta::desklog::logf_site((ch), &kDeskLogSite, (ms), __VA_ARGS__); \
    } while (0)
#define VDESK_ONCE(ch, slot, ...)     ::vanta::desklog::logf_once((ch), (slot), __VA_ARGS__)
#define VDESK_GLCHK(ch)               ::vanta::desklog::gl_check((ch), __FUNCTION__)
