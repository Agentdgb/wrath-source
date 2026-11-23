#include "DeskLog.h"
#include "platform/Win32Process.h"

#include <windows.h>
#include <shlobj.h>      // SHGetKnownFolderPath, FOLDERID_Desktop
#include <psapi.h>       // EnumProcessModules
#include <gl/GL.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

namespace vanta::desklog {

namespace {

// GL enums <gl/GL.h> (an OpenGL 1.1 header) predates. Same trick the rest of
// the codebase uses — see modules/render/arraylist/gl_ext.h.
constexpr GLenum kCurrentProgram      = 0x8B8D;
constexpr GLenum kFramebufferBinding  = 0x8CA6;
constexpr GLenum kActiveTexture       = 0x84E0;
constexpr GLenum kArrayBufferBinding  = 0x8894;
constexpr GLenum kVertexArrayBinding  = 0x85B5;
constexpr GLenum kMajorVersion        = 0x821B;
constexpr GLenum kMinorVersion        = 0x821C;
constexpr GLenum kContextProfileMask  = 0x9126;
constexpr GLenum kShadingLangVersion  = 0x8B8C;
constexpr GLenum kBlendSrcRgb         = 0x80C9;
constexpr GLenum kBlendDstRgb         = 0x80C8;
constexpr GLenum kFramebufferSrgb     = 0x8DB9;

constexpr int kMaxChannels = 8;
constexpr int kMaxCounters = 64;

struct Counter {
    char      name[48] = {};
    long long value    = 0;
};

struct JniFail {
    std::string line;
};

struct Chan {
    std::mutex          mx;
    std::FILE*          fp   = nullptr;
    std::string         name;              // basename, no extension
    std::string         path_utf8;
    unsigned long long  t0    = 0;
    unsigned long long  frame = 0;
    bool                open_attempted = false;

    unsigned long long  rl_next[kSlots]  = {};
    // Per-call-site rate limiting, keyed by a pointer the compiler makes unique.
    // Replaces hand-assigned slots, which silently suppressed each other.
    std::unordered_map<const void*, unsigned long long> rl_site_next;
    bool                once_done[kSlots] = {};

    Counter             counters[kMaxCounters] = {};
    int                 counter_count = 0;
    unsigned long long  counters_next_dump = 0;

    int                 jni_total = 0;
    int                 jni_ok    = 0;
    std::vector<JniFail> jni_fails;
};

Chan       g_chans[kMaxChannels];
std::mutex g_registry_mx;
int        g_chan_count = 0;

// ── path resolution ──────────────────────────────────────────────────────────

std::string wide_to_utf8(const wchar_t* w)
{
    if (!w) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

bool dir_exists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The candidate directories for debug logs: client logDir, then %TEMP%.
// Never dumps to the user's Desktop.
std::vector<std::wstring> log_candidates()
{
    std::vector<std::wstring> out;
    const std::wstring dir = vanta::platform::logDir();
    if (!dir.empty()) {
        out.emplace_back(dir);
    }

    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        out.emplace_back(tmp);

    return out;
}

bool open_file(Chan& c)
{
#ifndef _DEBUG
    (void)c;
    return false;
#else
    std::wstring wname;
    {
        const int need = MultiByteToWideChar(CP_UTF8, 0, c.name.c_str(), -1, nullptr, 0);
        if (need > 0) {
            wname.resize(static_cast<size_t>(need - 1));
            MultiByteToWideChar(CP_UTF8, 0, c.name.c_str(), -1, wname.data(), need);
        }
    }
    if (wname.empty()) wname = L"vanta_debug";

    for (const std::wstring& dir : log_candidates()) {
        if (!dir_exists(dir)) continue;
        const std::wstring full = dir + L"\\" + wname + L".txt";
        std::FILE* fp = nullptr;
        if (_wfopen_s(&fp, full.c_str(), L"w") == 0 && fp) {
            c.fp = fp;
            c.path_utf8 = wide_to_utf8(full.c_str());
            return true;
        }
    }
    return false;
#endif
}

// Caller must hold c.mx.
void ensure_open(Chan& c)
{
#ifndef _DEBUG
    c.open_attempted = true;
    return;
#else
    if (c.open_attempted) return;
    c.open_attempted = true;

    if (!open_file(c)) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "[vanta desklog] could not open a log file for channel '%s' "
                      "in any candidate directory\n", c.name.c_str());
        OutputDebugStringA(msg);
        return;
    }

    c.t0 = GetTickCount64();
    std::fprintf(c.fp,
        "=== gl desklog :: %s ===\n"
        "file      : %s\n"
        "pid       : %lu\n"
        "started   : GetTickCount64() = %llu\n"
        "timestamps: ms since the line above\n"
        "note      : lines are flushed individually, so a crash keeps the tail.\n\n",
        c.name.c_str(), c.path_utf8.c_str(),
        static_cast<unsigned long>(GetCurrentProcessId()),
        c.t0);
    std::fflush(c.fp);
#endif
}

Chan* get(Channel ch)
{
    if (ch.id < 0 || ch.id >= kMaxChannels) return nullptr;
    return &g_chans[ch.id];
}

// Caller must hold c.mx and have called ensure_open.
void write_line(Chan& c, const char* fmt, va_list ap)
{
    if (!c.fp) return;
    if (c.frame)
        std::fprintf(c.fp, "[%8llu ms][f%llu] ", GetTickCount64() - c.t0, c.frame);
    else
        std::fprintf(c.fp, "[%8llu ms] ", GetTickCount64() - c.t0);
    std::vfprintf(c.fp, fmt, ap);
    std::fputc('\n', c.fp);
    std::fflush(c.fp);
}

void write_linef(Chan& c, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    write_line(c, fmt, ap);
    va_end(ap);
}

} // namespace

// ── channel lifetime ─────────────────────────────────────────────────────────

Channel open(const char* basename)
{
#ifndef _DEBUG
    (void)basename;
    return Channel{ -1 };
#else
    if (!basename || !*basename) basename = "vanta_debug";

    std::lock_guard<std::mutex> lk(g_registry_mx);
    for (int i = 0; i < g_chan_count; ++i)
        if (g_chans[i].name == basename) return Channel{ i };

    if (g_chan_count >= kMaxChannels) {
        OutputDebugStringA("[vanta desklog] channel table full\n");
        return Channel{ -1 };
    }

    const int id = g_chan_count++;
    g_chans[id].name = basename;
    {
        std::lock_guard<std::mutex> clk(g_chans[id].mx);
        ensure_open(g_chans[id]);
    }
    return Channel{ id };
#endif
}

const char* path_of(Channel ch)
{
    Chan* c = get(ch);
    if (!c) return "";
    std::lock_guard<std::mutex> lk(c->mx);
    return c->path_utf8.c_str();
}

void flush(Channel ch)
{
    Chan* c = get(ch);
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    if (c->fp) std::fflush(c->fp);
}

void close_all()
{
    std::lock_guard<std::mutex> lk(g_registry_mx);
    for (int i = 0; i < g_chan_count; ++i) {
        std::lock_guard<std::mutex> clk(g_chans[i].mx);
        if (g_chans[i].fp) { std::fflush(g_chans[i].fp); std::fclose(g_chans[i].fp); }
        g_chans[i].fp = nullptr;
    }
}

// ── writing ──────────────────────────────────────────────────────────────────

void vlogf(Channel ch, const char* fmt, va_list ap)
{
    Chan* c = get(ch);
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    write_line(*c, fmt, ap);
}

void logf(Channel ch, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlogf(ch, fmt, ap);
    va_end(ap);
}

void logf_rl(Channel ch, int slot, unsigned min_ms, const char* fmt, ...)
{
    Chan* c = get(ch);
    if (!c) return;
    if (slot < 0 || slot >= kSlots) slot = 0;

    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    const unsigned long long now = GetTickCount64();
    if (now < c->rl_next[slot]) return;
    c->rl_next[slot] = now + min_ms;

    va_list ap; va_start(ap, fmt);
    write_line(*c, fmt, ap);
    va_end(ap);
}

void logf_site(Channel ch, const void* site, unsigned min_ms, const char* fmt, ...)
{
    Chan* c = get(ch);
    if (!c || !site) return;

    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    const unsigned long long now = GetTickCount64();
    auto it = c->rl_site_next.find(site);
    if (it != c->rl_site_next.end() && now < it->second) return;
    c->rl_site_next[site] = now + min_ms;

    va_list ap; va_start(ap, fmt);
    write_line(*c, fmt, ap);
    va_end(ap);
}

void logf_once(Channel ch, int slot, const char* fmt, ...)
{
    Chan* c = get(ch);
    if (!c) return;
    if (slot < 0 || slot >= kSlots) slot = 0;

    std::lock_guard<std::mutex> lk(c->mx);
    if (c->once_done[slot]) return;
    c->once_done[slot] = true;
    ensure_open(*c);

    va_list ap; va_start(ap, fmt);
    write_line(*c, fmt, ap);
    va_end(ap);
}

void section(Channel ch, const char* name)
{
    Chan* c = get(ch);
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    if (!c->fp) return;
    std::fprintf(c->fp, "\n──────────────── %s ────────────────\n", name ? name : "");
    std::fflush(c->fp);
}

unsigned long long frame_begin(Channel ch)
{
    Chan* c = get(ch);
    if (!c) return 0;
    std::lock_guard<std::mutex> lk(c->mx);
    return ++c->frame;
}

// ── structured dumps ─────────────────────────────────────────────────────────

void mat4(Channel ch, const char* label, const float m[16])
{
    Chan* c = get(ch);
    if (!c || !m) return;
    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    if (!c->fp) return;

    // Column-major storage (org.joml Matrix4f.get(float[]) / GL layout): element
    // (row r, column col) lives at m[col*4 + r]. Printed row by row so it reads
    // like the maths.
    write_linef(*c, "%s (column-major, printed as rows):", label ? label : "mat4");
    for (int r = 0; r < 4; ++r)
        std::fprintf(c->fp, "               [ %12.5f %12.5f %12.5f %12.5f ]\n",
                     m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]);
    std::fflush(c->fp);
}

void vec3f(Channel ch, const char* label, float x, float y, float z)
{
    logf(ch, "%s = (%.5f, %.5f, %.5f)", label ? label : "vec3", x, y, z);
}

void vec3d(Channel ch, const char* label, double x, double y, double z)
{
    logf(ch, "%s = (%.6f, %.6f, %.6f)", label ? label : "vec3", x, y, z);
}

void hexdump(Channel ch, const char* label, const void* data, unsigned long len)
{
    Chan* c = get(ch);
    if (!c) return;
    if (len > 4096) len = 4096;
    const auto* p = static_cast<const unsigned char*>(data);

    std::lock_guard<std::mutex> lk(c->mx);
    ensure_open(*c);
    if (!c->fp) return;

    write_linef(*c, "%s (%lu bytes%s):", label ? label : "hexdump", len,
                len == 4096 ? ", truncated" : "");
    if (!p) { std::fprintf(c->fp, "               <null>\n"); std::fflush(c->fp); return; }

    for (unsigned long off = 0; off < len; off += 16) {
        char hex[16 * 3 + 1] = {};
        char asc[17]         = {};
        int  hp = 0;
        for (unsigned long i = 0; i < 16; ++i) {
            if (off + i < len) {
                const unsigned char b = p[off + i];
                hp += std::snprintf(hex + hp, sizeof(hex) - static_cast<size_t>(hp), "%02X ", b);
                asc[i] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
            } else {
                hp += std::snprintf(hex + hp, sizeof(hex) - static_cast<size_t>(hp), "   ");
                asc[i] = ' ';
            }
        }
        std::fprintf(c->fp, "               %08lX  %s |%s|\n", off, hex, asc);
    }
    std::fflush(c->fp);
}

// ── counters ─────────────────────────────────────────────────────────────────

void bump(Channel ch, const char* name, long long by)
{
    Chan* c = get(ch);
    if (!c || !name) return;
    std::lock_guard<std::mutex> lk(c->mx);
    for (int i = 0; i < c->counter_count; ++i) {
        if (std::strcmp(c->counters[i].name, name) == 0) {
            c->counters[i].value += by;
            return;
        }
    }
    if (c->counter_count >= kMaxCounters) return;
    Counter& n = c->counters[c->counter_count++];
    std::snprintf(n.name, sizeof(n.name), "%s", name);
    n.value = by;
}

namespace {
// Caller must hold c.mx.
void dump_counters_locked(Chan& c)
{
    ensure_open(c);
    if (!c.fp) return;
    bool any = false;
    for (int i = 0; i < c.counter_count; ++i)
        if (c.counters[i].value) { any = true; break; }
    if (!any) return;

    write_linef(c, "counters:");
    for (int i = 0; i < c.counter_count; ++i) {
        if (!c.counters[i].value) continue;
        std::fprintf(c.fp, "               %-32s %lld\n",
                     c.counters[i].name, c.counters[i].value);
        c.counters[i].value = 0;
    }
    std::fflush(c.fp);
}
} // namespace

void dump_counters(Channel ch)
{
    Chan* c = get(ch);
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    dump_counters_locked(*c);
}

void dump_counters_rl(Channel ch, unsigned min_ms)
{
    Chan* c = get(ch);
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    const unsigned long long now = GetTickCount64();
    if (now < c->counters_next_dump) return;
    c->counters_next_dump = now + min_ms;
    dump_counters_locked(*c);
}

// ── OpenGL helpers ───────────────────────────────────────────────────────────

namespace {
const char* gl_err_name(GLenum e)
{
    switch (e) {
        case GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_STACK_OVERFLOW:    return "GL_STACK_OVERFLOW";
        case GL_STACK_UNDERFLOW:   return "GL_STACK_UNDERFLOW";
        case GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
        case 0x0506:               return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default:                   return "GL_?";
    }
}
} // namespace

int gl_check(Channel ch, const char* where)
{
    int n = 0;
    for (;;) {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        ++n;
        logf(ch, "GL ERROR at %s: 0x%04X %s", where ? where : "?",
             static_cast<unsigned>(e), gl_err_name(e));
        if (n > 32) break;   // a stuck context would otherwise spin forever
    }
    return n;
}

void gl_state(Channel ch, const char* where)
{
    GLint depth_func = 0, depth_mask = 0, prog = 0, fbo = 0, tex = 0;
    GLint vp[4] = {}, sc[4] = {};
    GLint blend_src = 0, blend_dst = 0, active_tex = 0, vao = 0, vbo = 0;

    glGetIntegerv(GL_DEPTH_FUNC,          &depth_func);
    glGetIntegerv(GL_DEPTH_WRITEMASK,     &depth_mask);
    glGetIntegerv(GL_VIEWPORT,            vp);
    glGetIntegerv(GL_SCISSOR_BOX,         sc);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,  &tex);
    glGetIntegerv(kCurrentProgram,        &prog);
    glGetIntegerv(kFramebufferBinding,    &fbo);
    glGetIntegerv(kBlendSrcRgb,           &blend_src);
    glGetIntegerv(kBlendDstRgb,           &blend_dst);
    glGetIntegerv(kActiveTexture,         &active_tex);
    glGetIntegerv(kVertexArrayBinding,    &vao);
    glGetIntegerv(kArrayBufferBinding,    &vbo);

    const GLboolean depth_on   = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blend_on   = glIsEnabled(GL_BLEND);
    const GLboolean cull_on    = glIsEnabled(GL_CULL_FACE);
    const GLboolean scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean srgb_on    = glIsEnabled(kFramebufferSrgb);

    logf(ch,
         "glstate @%s: depth[test=%d func=0x%04X mask=%d] blend[on=%d %04X->%04X] "
         "cull=%d scissor[on=%d %d,%d %dx%d] srgb=%d vp=%d,%d %dx%d "
         "prog=%d fbo=%d vao=%d vbo=%d tex2d=%d activeTex=0x%04X",
         where ? where : "?",
         static_cast<int>(depth_on), static_cast<unsigned>(depth_func), depth_mask,
         static_cast<int>(blend_on), static_cast<unsigned>(blend_src), static_cast<unsigned>(blend_dst),
         static_cast<int>(cull_on),
         static_cast<int>(scissor_on), sc[0], sc[1], sc[2], sc[3],
         static_cast<int>(srgb_on),
         vp[0], vp[1], vp[2], vp[3],
         prog, fbo, vao, vbo, tex, static_cast<unsigned>(active_tex));

    // Reading state must never itself leave an error queued: with Sodium's
    // synchronous debug output that is an in-driver abort. Some of the enums
    // above do not exist on a 1.1-only context, so a drain here is not optional.
    while (glGetError() != GL_NO_ERROR) { /* drain */ }
}

void banner(Channel ch, bool have_gl_context)
{
    section(ch, "environment");

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    logf(ch, "process   : %s (pid %lu)", wide_to_utf8(exe).c_str(),
         static_cast<unsigned long>(GetCurrentProcessId()));

    if (have_gl_context) {
        auto s = [](GLenum e) -> const char* {
            const GLubyte* v = glGetString(e);
            return v ? reinterpret_cast<const char*>(v) : "<null>";
        };
        logf(ch, "GL_VENDOR   : %s", s(GL_VENDOR));
        logf(ch, "GL_RENDERER : %s", s(GL_RENDERER));
        logf(ch, "GL_VERSION  : %s", s(GL_VERSION));
        logf(ch, "GLSL        : %s", s(kShadingLangVersion));

        GLint maj = 0, min_ = 0, profile = 0;
        glGetIntegerv(kMajorVersion, &maj);
        glGetIntegerv(kMinorVersion, &min_);
        if (maj >= 3 && (maj > 3 || min_ >= 2))
            glGetIntegerv(kContextProfileMask, &profile);
        logf(ch, "context     : %d.%d profile_mask=0x%X (core=%d compat=%d)",
             maj, min_, static_cast<unsigned>(profile),
             (profile & 0x1) ? 1 : 0, (profile & 0x2) ? 1 : 0);

        while (glGetError() != GL_NO_ERROR) { /* drain */ }
    } else {
        logf(ch, "GL strings  : skipped (no current context at banner time)");
    }

    // Which ICD / graphics-related native modules are in the process. This says
    // whose OpenGL implementation you are actually hooking. It says NOTHING
    // about Sodium or Iris — those are Java mods with no native module.
    HMODULE mods[512];
    DWORD   needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        logf(ch, "native graphics modules loaded:");
        static const wchar_t* kInteresting[] = {
            L"opengl32", L"nvoglv", L"atio6ax", L"atioglxx", L"ig9icd", L"igdgl",
            L"igxelpicd", L"vulkan-1", L"d3d11", L"dxgi", L"gdi32",
        };
        for (DWORD i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
            std::wstring lower(path);
            for (auto& wc : lower) wc = static_cast<wchar_t>(towlower(wc));
            for (const wchar_t* k : kInteresting) {
                if (lower.find(k) != std::wstring::npos) {
                    logf(ch, "               %s  base=0x%p",
                         wide_to_utf8(path).c_str(), static_cast<void*>(mods[i]));
                    break;
                }
            }
        }
    } else {
        logf(ch, "native graphics modules: EnumProcessModules failed (%lu)", GetLastError());
    }
}

// ── JNI resolution reporting ─────────────────────────────────────────────────

void jni_lookup(Channel ch, const char* kind, const char* owner,
                const char* name, const char* desc, bool ok)
{
    Chan* c = get(ch);
    if (!c) return;

    char line[512];
    std::snprintf(line, sizeof(line), "%-6s %s.%s %s",
                  kind  ? kind  : "?",
                  owner ? owner : "?",
                  name  ? name  : "?",
                  desc  ? desc  : "");

    {
        std::lock_guard<std::mutex> lk(c->mx);
        ++c->jni_total;
        if (ok) ++c->jni_ok;
        else    c->jni_fails.push_back(JniFail{ line });
    }

    logf(ch, "jni %-4s %s", ok ? "OK" : "FAIL", line);
}

void jni_summary(Channel ch)
{
    Chan* c = get(ch);
    if (!c) return;

    int total = 0, ok = 0;
    std::vector<JniFail> fails;
    {
        std::lock_guard<std::mutex> lk(c->mx);
        total = c->jni_total;
        ok    = c->jni_ok;
        fails = c->jni_fails;
    }

    section(ch, "jni resolution summary");
    logf(ch, "resolved %d / %d lookups", ok, total);
    if (fails.empty()) {
        logf(ch, "no failures — every class/method/field the module needs exists on this version");
        return;
    }
    logf(ch, "%zu FAILURES — these are the ones to chase first:", fails.size());
    for (const JniFail& f : fails)
        logf(ch, "    %s", f.line.c_str());
}

} // namespace vanta::desklog

