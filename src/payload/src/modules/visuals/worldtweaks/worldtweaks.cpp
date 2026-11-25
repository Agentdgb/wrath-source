#include "core/NativeCallbacks.h"
// worldtweaks.cpp — client-side time-of-day override + custom overlay particles.
//
// Two features, both client-side only:
//   * Time — applied via ClientWorld.setTime(worldAge, timeOfDay, false) each
//     frame (method_29089, the exact call the WorldTimeUpdate packet uses).
//     shouldTick=false freezes the client's own time advance so our value
//     persists to the sky. The server's real time is untouched.
//   * Particles — our OWN particle system. Particles live at world coordinates
//     and are simulated on the game thread; render_text projects each one to the
//     screen through the shared hitfx camera capture (same math the ESP uses)
//     and draws a soft glow on the ImGui foreground draw list. They are NOT
//     Minecraft particles: they don't touch MC's particle system, so they show
//     regardless of the video Particles setting, and they are drawn on the
//     overlay (no depth occlusion — they glow through terrain, which for glowing
//     particles reads better anyway).
//   * Sky-flash guard — a JNIHook detour on WorldTimeUpdateS2CPacket.timeOfDay()
//     so the server's once-a-second time sync writes OUR time while the override
//     is on (see install_time_hook). Needed because this build has no Java hook
//     bridge, so PacketReceiveEvent never fires and the packet cannot be
//     cancelled; without it the sky showed the real time for one frame a second.
//
// Mappings are MC 1.21.11 yarn intermediary, verified against
// tools/genmaps/.cache/1.21.11/yarn_merged.tiny (not guessed):
//   MinecraftClient class_310  getInstance=method_1551, world=field_1687, player=field_1724
//   World           class_1937 properties=field_9232 (Lclass_5269;)
//   ClientWorld     class_638  setTime=method_29089 (JJZ)V
//   ClientWorld.Properties class_638$class_5271  time=field_24438(J) timeOfDay=field_24439(J)
//   Entity          class_1297 getX=method_23317 getY=method_23318 getZ=method_23321 ()D

#include "worldtweaks.h"

#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"

#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"

#include <imgui.h>
#include "modules/render/world_overlay_draw_scope.h"
#include "modules/render/world_overlay_view.h"
#include "util/Log.h"
#include <jnihook.h>
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

namespace dt::modules::worldtweaks {

namespace cap = dt::modules::hitfx::capture;
namespace hf  = dt::modules::hitfx;

settings          g_settings;
std::atomic<bool> g_enabled{false};

namespace {

// ── debug log ────────────────────────────────────────────────────────────────
std::mutex         g_log_mx;
FILE*              g_log_fp = nullptr;
bool               g_log_tried = false;
unsigned long long g_log_t0 = 0;

bool wlog_try_open(const char* p) {
    FILE* f = nullptr;
    if (fopen_s(&f, p, "wb") == 0 && f) { g_log_fp = f; return true; }
    return false;
}
void wlog_open_once() {
    if (g_log_tried) return;
    g_log_tried = true;
    char buf[512]; size_t needed = 0;
    if (getenv_s(&needed, buf, sizeof(buf), "USERPROFILE") == 0 && needed) {
        std::string p(buf); p += "\\gl_world_trace.txt"; wlog_try_open(p.c_str());
    }
    if (!g_log_fp && getenv_s(&needed, buf, sizeof(buf), "USERPROFILE") == 0 && needed) {
        std::string p(buf); p += "\\gl_world_trace.txt"; wlog_try_open(p.c_str());
    }
    if (!g_log_fp && getenv_s(&needed, buf, sizeof(buf), "TEMP") == 0 && needed) {
        std::string p(buf); p += "\\gl_world_trace.txt"; wlog_try_open(p.c_str());
    }
    if (g_log_fp) {
        g_log_t0 = GetTickCount64();
        std::fprintf(g_log_fp,
            "=== gl_world_trace debug log — session start ===\n"
            "pid=%lu   Reopen the file to refresh; Notepad won't auto-tail.\n\n",
            (unsigned long)GetCurrentProcessId());
        std::fflush(g_log_fp);
    }
}
void wlog(const char* fmt, ...) {
    if (!g_settings.debug) return;
    std::lock_guard lk(g_log_mx);
    wlog_open_once();
    if (!g_log_fp) return;
    std::fprintf(g_log_fp, "[%8llu ms] ", (unsigned long long)(GetTickCount64() - g_log_t0));
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_log_fp, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log_fp);
    std::fflush(g_log_fp);
}

// ── tiny PRNG (workflow scripts ban rand; module C++ is free to use it) ──────
std::uint64_t g_rng = 0x9E3779B97F4A7C15ull;
inline std::uint64_t rng_next() {
    std::uint64_t x = g_rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; g_rng = x; return x;
}
inline float rand01()           { return (float)((rng_next() >> 11) * (1.0 / 9007199254740992.0)); }
inline float rand_sym(float m)  { return (rand01() * 2.f - 1.f) * m; }
inline float rand_rng(float a, float b) { return a + rand01() * (b - a); }

// ── JNI cache ────────────────────────────────────────────────────────────────
struct Cache {
    jclass    mc_class = nullptr;
    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_world = nullptr;
    jfieldID  mc_player = nullptr;

    jclass    world_base = nullptr;   // class_1937 (owns the properties field)
    jfieldID  fld_properties = nullptr;

    jclass    world_client = nullptr; // class_638
    jmethodID m_setTime = nullptr;    // method_29089 setTime(JJZ)V

    jclass    ent_class = nullptr;    // class_1297
    jmethodID ent_getX = nullptr, ent_getY = nullptr, ent_getZ = nullptr;

    // Resolved lazily from the properties object's runtime class (ClientWorld.Properties).
    jfieldID  fld_time      = nullptr; // J (worldAge, field_24438)
    jfieldID  fld_timeOfDay = nullptr; // J (field_24439)
} C;

// WorldTimeUpdateS2CPacket (class_2761), discovered by class NAME from the first
// matching received packet so the cached class is from the packets' own
// classloader (see wants_cancel_time_packet). java.lang.Class.getName is cached
// alongside for the discovery scan.
jclass    g_timeupdate_class = nullptr;
jmethodID g_class_getName    = nullptr;
bool      g_timepkt_logged   = false;

jclass global_class(JNIEnv* env, const char* binName) {
    jclass k = dt::jvm::findClassAnyLoader(env, binName);
    if (!k) return nullptr;
    jclass g = static_cast<jclass>(env->NewGlobalRef(k));
    env->DeleteLocalRef(k);
    return g;
}

void resolve_all(JNIEnv* env) {
    if (!C.mc_class)  C.mc_class = global_class(env, "net/minecraft/class_310");
    if (C.mc_class) {
        if (!C.mc_getInstance) { C.mc_getInstance = env->GetStaticMethodID(C.mc_class, "method_1551", "()Lnet/minecraft/class_310;"); if (env->ExceptionCheck()) env->ExceptionClear(); }
        if (!C.mc_world)       { C.mc_world = env->GetFieldID(C.mc_class, "field_1687", "Lnet/minecraft/class_638;"); if (env->ExceptionCheck()) env->ExceptionClear(); }
        if (!C.mc_player)      { C.mc_player = env->GetFieldID(C.mc_class, "field_1724", "Lnet/minecraft/class_746;"); if (env->ExceptionCheck()) env->ExceptionClear(); }
    }

    if (!C.world_base) C.world_base = global_class(env, "net/minecraft/class_1937");
    if (C.world_base && !C.fld_properties) {
        C.fld_properties = env->GetFieldID(C.world_base, "field_9232", "Lnet/minecraft/class_5269;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (!C.world_client) C.world_client = global_class(env, "net/minecraft/class_638");
    if (C.world_client && !C.m_setTime) {
        C.m_setTime = env->GetMethodID(C.world_client, "method_29089", "(JJZ)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (!C.ent_class) C.ent_class = global_class(env, "net/minecraft/class_1297");
    if (C.ent_class) {
        if (!C.ent_getX) { C.ent_getX = env->GetMethodID(C.ent_class, "method_23317", "()D"); if (env->ExceptionCheck()) env->ExceptionClear(); }
        if (!C.ent_getY) { C.ent_getY = env->GetMethodID(C.ent_class, "method_23318", "()D"); if (env->ExceptionCheck()) env->ExceptionClear(); }
        if (!C.ent_getZ) { C.ent_getZ = env->GetMethodID(C.ent_class, "method_23321", "()D"); if (env->ExceptionCheck()) env->ExceptionClear(); }
    }

    // NOTE: the time-packet class is intentionally NOT resolved here. A class
    // found via findClassAnyLoader can come from a different classloader than the
    // one that actually loaded the received packet objects (Fabric knot loader),
    // and IsInstanceOf across loaders is always false — that silently broke the
    // cancel. It is discovered by name from the first matching packet instead
    // (see wants_cancel_time_packet), so the cached class is guaranteed to be the
    // packets' own class.
}

jobject get_client(JNIEnv* env) {
    if (!C.mc_class || !C.mc_getInstance) return nullptr;
    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return mc;
}

void resolve_props_fields(JNIEnv* env, jobject propsObj) {
    if (C.fld_time && C.fld_timeOfDay) return;
    jclass k = env->GetObjectClass(propsObj);
    if (!k) return;
    if (!C.fld_time)      { C.fld_time      = env->GetFieldID(k, "field_24438", "J"); if (env->ExceptionCheck()) env->ExceptionClear(); }
    if (!C.fld_timeOfDay) { C.fld_timeOfDay = env->GetFieldID(k, "field_24439", "J"); if (env->ExceptionCheck()) env->ExceptionClear(); }
    env->DeleteLocalRef(k);
}

// ── engine state: time ───────────────────────────────────────────────────────
double disp_time = 0.0;   bool time_seeded = false;
int    last_slider = INT32_MIN;
// The time the override wants the sky to show, published for the packet detour
// (hkPacketTimeOfDay below). -1 while the override is off. Written by
// apply_time and read by the detour, both on the game thread; atomic so a
// toggle can never tear.
std::atomic<long long> g_override_tod{-1};

// ── engine state: particle pool ──────────────────────────────────────────────
// Simulated on the game thread (tick → simulate_particles), drawn on the render
// thread (render_text). Both run on the wglSwapBuffers thread in this build, but
// the mutex keeps the pool safe even if that ever stops being true.
struct CParticle {
    float x, y, z;        // world position
    float vx, vy, vz;     // velocity, blocks/sec
    float age, life;      // seconds
    float seed;           // 0..1 phase for twinkle / hue offset
    float sizeMul;        // per-particle size variation
    std::uint8_t kind;    // AmbientMode this particle was spawned as
};
constexpr int      kMaxParticles = 3000;
std::mutex         g_pool_mx;
std::vector<CParticle> g_pool;
float              particle_accum = 0.f;   // fractional spawn carry

// ── advanced debug bookkeeping ───────────────────────────────────────────────
unsigned long long g_last_status_ms = 0;
int                g_stat_spawned = 0;      // spawned since last status
bool               g_logged_resolution = false;
std::atomic<int>   g_timepkt_cancelled{0};  // time packets dropped since last status
int                g_ext_changes = 0;       // external time resets seen since last status
std::atomic<int>   g_pkt_seen{0};           // packets the cancel-check ran on, since last status
int                g_names_dumped = 0;      // diagnostic: how many packet class names logged so far

void hsv2rgb(float h, float s, float v, float& r, float& g, float& b) {
    h -= std::floor(h);
    const float i = std::floor(h * 6.f);
    const float f = h * 6.f - i;
    const float p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (((int)i) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default:r = v; g = p; b = q; break;
    }
}

inline ImU32 rgba_u32(float r, float g, float b, float a) {
    auto cl = [](float v) { int i = (int)(v * 255.f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
    return IM_COL32(cl(r), cl(g), cl(b), cl(a));
}

// Soft radial glow: a triangle fan with the centre at full colour and the rim
// fully TRANSPARENT, so alpha falls off smoothly across the disc. This is what a
// glow actually looks like — a flat AddCircleFilled gives a hard-edged token
// disc instead. rx/ry allow vertical elongation (flames, souls). The backend
// honours VtxOffset (GL ≥ 3.2) so writing raw verts here is safe at any count.
void soft_glow(ImDrawList* dl, ImVec2 c, float rx, float ry,
               float r, float g, float b, float a, int segs = 20) {
    if (rx < 0.5f || ry < 0.5f || a <= 0.004f) return;
    const ImU32 c_in  = rgba_u32(r, g, b, a);
    const ImU32 c_out = rgba_u32(r, g, b, 0.f);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(segs * 3, segs + 1);
    const unsigned int base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, c_in);
    for (int i = 0; i < segs; ++i) {
        const float ang = (float)i / (float)segs * 6.2831853f;
        dl->PrimWriteVtx(ImVec2(c.x + std::cos(ang) * rx, c.y + std::sin(ang) * ry), uv, c_out);
    }
    for (int i = 0; i < segs; ++i) {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + ((i + 1) % segs)));
    }
}

// One tapered spike: a bright wide base at the centre fading to a transparent
// tip. Builds star / snowflake arms.
void spike(ImDrawList* dl, ImVec2 c, float ang, float len, float w,
           float r, float g, float b, float a) {
    if (len < 0.5f || a <= 0.004f) return;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const ImVec2 tip(c.x + ca * len, c.y + sa * len);
    const ImVec2 b1(c.x - sa * w, c.y + ca * w);
    const ImVec2 b2(c.x + sa * w, c.y - ca * w);
    const ImU32 c_base = rgba_u32(r, g, b, a);
    const ImU32 c_tip  = rgba_u32(r, g, b, 0.f);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(3, 3);
    const unsigned int base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(b1, uv, c_base);
    dl->PrimWriteVtx(b2, uv, c_base);
    dl->PrimWriteVtx(tip, uv, c_tip);
    dl->PrimWriteIdx((ImDrawIdx)base);
    dl->PrimWriteIdx((ImDrawIdx)(base + 1));
    dl->PrimWriteIdx((ImDrawIdx)(base + 2));
}

// A star of `points` tapered spikes, rotated by `rot`.
void star(ImDrawList* dl, ImVec2 c, int points, float len, float w, float rot,
          float r, float g, float b, float a) {
    for (int k = 0; k < points; ++k)
        spike(dl, c, rot + (float)k / (float)points * 6.2831853f, len, w, r, g, b, a);
}

// Per-preset base life in seconds.
float kind_life(AmbientMode m) {
    switch (m) {
        case AmbientMode::Fireflies: return rand_rng(3.0f, 6.0f);
        case AmbientMode::Embers:    return rand_rng(1.6f, 3.0f);
        case AmbientMode::Snow:      return rand_rng(4.0f, 7.0f);
        case AmbientMode::Sparkles:  return rand_rng(0.8f, 1.7f);
        case AmbientMode::Magic:     return rand_rng(4.0f, 7.0f);
        case AmbientMode::Souls:     return rand_rng(4.0f, 6.5f);
        default:                     return 3.0f;
    }
}

// Spawn one particle of `mode` around the player at (px,py,pz), radius r.
void spawn_one(AmbientMode mode, float px, float py, float pz, float r) {
    if ((int)g_pool.size() >= kMaxParticles) return;
    CParticle p{};
    p.kind    = (std::uint8_t)mode;
    p.age     = 0.f;
    p.life    = kind_life(mode);
    p.seed    = rand01();
    p.sizeMul = rand_rng(0.7f, 1.35f);

    switch (mode) {
        case AmbientMode::Fireflies:
            p.x = px + rand_sym(r); p.y = py + rand01() * r * 0.55f; p.z = pz + rand_sym(r);
            p.vx = rand_sym(0.25f); p.vy = rand_sym(0.12f); p.vz = rand_sym(0.25f);
            break;
        case AmbientMode::Embers:
            p.x = px + rand_sym(r); p.y = py + rand01() * 1.2f; p.z = pz + rand_sym(r);
            p.vx = rand_sym(0.15f); p.vy = rand_rng(0.8f, 1.7f); p.vz = rand_sym(0.15f);
            break;
        case AmbientMode::Snow:
            p.x = px + rand_sym(r); p.y = py + rand_rng(r * 0.5f, r); p.z = pz + rand_sym(r);
            p.vx = rand_sym(0.1f); p.vy = -rand_rng(1.1f, 2.0f); p.vz = rand_sym(0.1f);
            break;
        case AmbientMode::Sparkles:
            p.x = px + rand_sym(r); p.y = py + rand01() * r * 0.55f; p.z = pz + rand_sym(r);
            p.vx = rand_sym(0.06f); p.vy = rand_sym(0.06f); p.vz = rand_sym(0.06f);
            break;
        case AmbientMode::Magic: {
            const float a = rand01() * 6.2831853f;
            const float rad = r * 0.4f + rand01() * r * 0.35f;
            p.x = px + std::cos(a) * rad; p.y = py + rand01() * r * 0.45f; p.z = pz + std::sin(a) * rad;
            // tangential velocity → orbiting look
            p.vx = -std::sin(a) * 0.8f; p.vy = rand_rng(0.05f, 0.25f); p.vz = std::cos(a) * 0.8f;
            break;
        }
        case AmbientMode::Souls:
            p.x = px + rand_sym(r); p.y = py + rand01() * 1.5f; p.z = pz + rand_sym(r);
            p.vx = rand_sym(0.1f); p.vy = rand_rng(0.45f, 0.9f); p.vz = rand_sym(0.1f);
            break;
        default: return;
    }
    g_pool.push_back(p);
}

// Integrate + apply per-preset motion; cull dead. dt in seconds.
void integrate(CParticle& p, float dt) {
    const AmbientMode m = (AmbientMode)p.kind;
    switch (m) {
        case AmbientMode::Fireflies:
            // slow drifting wander, gentle bob
            p.vx += std::sin(p.age * 1.3f + p.seed * 6.28f) * 0.4f * dt;
            p.vz += std::cos(p.age * 1.1f + p.seed * 5.0f) * 0.4f * dt;
            p.vy += std::sin(p.age * 2.0f + p.seed * 6.28f) * 0.3f * dt;
            p.vx *= 0.96f; p.vy *= 0.96f; p.vz *= 0.96f;
            break;
        case AmbientMode::Embers:
            p.vy += 0.6f * dt;                        // buoyancy
            p.vx += rand_sym(0.4f) * dt; p.vz += rand_sym(0.4f) * dt;
            p.vx *= 0.97f; p.vz *= 0.97f;
            break;
        case AmbientMode::Snow:
            p.x += std::sin(p.age * 1.2f + p.seed * 6.28f) * 0.5f * dt;
            p.z += std::cos(p.age * 0.9f + p.seed * 3.0f) * 0.5f * dt;
            break;
        case AmbientMode::Sparkles:
            p.vx *= 0.94f; p.vy *= 0.94f; p.vz *= 0.94f;
            break;
        case AmbientMode::Magic: {
            // rotate horizontal velocity about Y → spiral
            const float ang = 1.6f * dt;
            const float c = std::cos(ang), s = std::sin(ang);
            const float nvx = p.vx * c - p.vz * s;
            const float nvz = p.vx * s + p.vz * c;
            p.vx = nvx; p.vz = nvz;
            break;
        }
        case AmbientMode::Souls:
            p.x += std::sin(p.age * 3.0f + p.seed * 6.28f) * 0.4f * dt;
            p.z += std::cos(p.age * 2.4f + p.seed * 2.0f) * 0.3f * dt;
            break;
        default: break;
    }
    p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
    p.age += dt;
}

// Spawn + simulate. Called from tick with the player position and frame delta.
void simulate_particles(float px, float py, float pz, float dt_ms) {
    const AmbientMode mode = (AmbientMode)g_settings.ambient_mode;
    const float dt = dt_ms / 1000.f;

    std::lock_guard<std::mutex> lk(g_pool_mx);

    // Integrate + cull existing (of any kind, so switching modes drains cleanly).
    for (size_t i = 0; i < g_pool.size();) {
        integrate(g_pool[i], dt);
        if (g_pool[i].age >= g_pool[i].life) {
            g_pool[i] = g_pool.back();
            g_pool.pop_back();
        } else {
            ++i;
        }
    }

    // Spawn new for the current mode.
    if (mode != AmbientMode::Off) {
        const float r = (float)g_settings.ambient_radius;
        particle_accum += (float)g_settings.ambient_density * dt;
        int n = (int)particle_accum;
        if (n > 40) n = 40;              // per-frame safety cap
        particle_accum -= n;
        for (int i = 0; i < n; ++i) { spawn_one(mode, px, py, pz, r); ++g_stat_spawned; }
    }
}

void apply_time(JNIEnv* env, jobject world, jobject propsObj, float dt_ms) {
    if (!g_settings.time_enabled) {
        time_seeded = false;
        g_override_tod.store(-1, std::memory_order_relaxed);
        return;
    }
    resolve_props_fields(env, propsObj);
    if (!C.fld_timeOfDay) return;

    // Diagnostic: before we (re)apply, read what the world's timeOfDay ACTUALLY
    // is now. If it drifted far from what we wrote last frame, something external
    // moved it between frames (a packet we didn't cancel, a respawn, tickTime
    // advancing because shouldTick got re-enabled...) — that is the sky-flash.
    if (g_settings.debug && time_seeded) {
        const long long cur = env->GetLongField(propsObj, C.fld_timeOfDay);
        const long long expc = (long long)disp_time;                 // what we wrote last frame
        long long d = ((cur - expc) % 24000 + 24000) % 24000;
        if (d > 12000) d = 24000 - d;                                // circular distance
        if (d > 60) {                                                // > ~3 ticks off = external
            ++g_ext_changes;
            wlog("EXTERNAL time change: wrote=%lld worldNow=%lld delta=%lld speed=%.1f timePktClass=%d",
                 expc, cur, d, g_settings.time_speed, g_timeupdate_class != nullptr);
        }
    }

    const int slider = g_settings.time_ticks < 0 ? 0
                     : (g_settings.time_ticks > 24000 ? 24000 : g_settings.time_ticks);
    if (!time_seeded) { disp_time = slider; last_slider = slider; time_seeded = true; }
    if (slider != last_slider) { disp_time = slider; last_slider = slider; }

    if (g_settings.time_speed > 0.f) {
        disp_time = std::fmod(disp_time + (double)g_settings.time_speed * (dt_ms / 1000.0), 24000.0);
        if (disp_time < 0.0) disp_time += 24000.0;
    } else {
        disp_time = slider;
    }

    // Publish before writing: from here on any WorldTimeUpdate the server sends
    // is rewritten to this value by the packet detour, so the sky never sees
    // the server's time between two of our frames.
    g_override_tod.store((long long)disp_time, std::memory_order_relaxed);

    if (C.m_setTime && C.fld_time) {
        const jlong worldAge = env->GetLongField(propsObj, C.fld_time);
        env->CallVoidMethod(world, C.m_setTime, worldAge, (jlong)(long long)disp_time, JNI_FALSE);
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else {
        env->SetLongField(propsObj, C.fld_timeOfDay, (jlong)(long long)disp_time);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

// ── WorldTimeUpdateS2CPacket.timeOfDay() detour: the sky-flash fix ──────────
//
// The flash: once a second the server's WorldTimeUpdate lands on the game
// thread (runTasks, at the top of runGameLoop) and ClientWorld.setTime writes
// the real time. Our per-frame correction runs from GameLoopEvent, which this
// build pumps at wglSwapBuffers — AFTER the world was drawn. So on that frame
// the sky shows the server's time and we only put ours back on the next swap:
// a one-frame flash, once a second, for as long as the override is on.
// PacketReceiveEvent cannot stop it because the Java hook bridge is not
// embedded in this tree (HookBridgeClass.cpp logs "shim absent" at boot), so
// wants_cancel_time_packet never runs — pkts_seen_last_s stays 0.
//
// The fix hooks the packet itself. onWorldTimeUpdate reads packet.timeOfDay()
// (a record accessor on 1.21.2+, getTimeOfDay() on 1.21/1.21.1) and hands it
// to ClientWorld.setTime; with that accessor detoured to return OUR time while
// the override is on, the handler writes our value and the server's never
// reaches the sky. Everything else the handler does (worldAge, tickDayTime)
// stays vanilla, and the per-frame write in apply_time still carries TimeSpeed
// and the initial application.
//
// Why this class: it is a three-field record (five methods), so JNIHook's
// sibling copy is trivial; it is shared with no other hook (Backtrack owns
// class_634, the handler); and tools/jnihook_copy_audit.py PASSES for
// comp_3220 on 1.21.4 / 1.21.11 and for method_11873 on 1.21 — nothing the
// original executes inside the copy names the class in a foreign descriptor.
// The previous attempt detoured World.getTimeOfDay (class_1937, a huge class
// whose copy never attached); it is gone.
//
// Names verified in tools/genmaps/.cache/<ver>/yarn_merged.tiny:
//     class_2761  comp_3220    ()J   timeOfDay     1.21.2 .. 1.21.11
//     class_2761  method_11873 ()J   getTimeOfDay  1.21, 1.21.1
//
// The class is resolved through the loader that owns MinecraftClient
// (Class.forName(name, false, class_310.getClassLoader())): the same name is
// also reachable from the app loader (the game jar sits on the launcher
// classpath), and hooking THAT copy would attach cleanly and change nothing —
// the trap the packet-cancel path fell into with IsInstanceOf.
jclass             g_timepkt_hook_class = nullptr;   // class_2761, global ref (the REAL class)
jmethodID          ORIG_pkt_timeOfDay   = nullptr;   // the copy's untouched accessor
std::atomic<bool>  g_timehook_installed{false};
unsigned long long g_timehook_next_try_ms = 0;
bool               g_timehook_logged_wait = false;

jlong JNICALL hkPacketTimeOfDay(JNIEnv* env, jobject thiz) {
    dt::NativeCallback callbackLifetime;
    const long long ours = g_override_tod.load(std::memory_order_relaxed);
    if (ours >= 0) return static_cast<jlong>(ours);
    if (!ORIG_pkt_timeOfDay || !g_timepkt_hook_class) return 0;
    // Override off: the packet's own value. A pending exception (none is
    // expected from a field read) propagates to the handler exactly as vanilla.
    return env->CallNonvirtualLongMethod(thiz, g_timepkt_hook_class, ORIG_pkt_timeOfDay);
}

// SEH-guarded attach in its own POD-only function (MSVC forbids __try in a frame
// that holds C++ destructors) — mirrors the other JNIHook users.
static int attach_time_hook_seh(jmethodID mid) {
    jnihook_result_t ar = JNIHOOK_ERR_JVMTI_OPERATION;
    __try { ar = JNIHook_Attach(mid, reinterpret_cast<void*>(&hkPacketTimeOfDay), &ORIG_pkt_timeOfDay); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ar = JNIHOOK_ERR_JVMTI_OPERATION; }
    return (int)ar;
}

// Class.forName(dottedName, initialize=false, anchor.getClassLoader()).
// Returns a LOCAL ref, or null with any exception cleared.
jclass find_class_beside(JNIEnv* env, jclass anchor, const char* dotted_name) {
    if (!env || !anchor) return nullptr;
    jclass class_class = env->GetObjectClass(anchor);            // java.lang.Class
    if (!class_class) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jmethodID get_loader = env->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID for_name   = env->GetStaticMethodID(class_class, "forName",
                               "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass result = nullptr;
    if (get_loader && for_name) {
        jobject loader = env->CallObjectMethod(anchor, get_loader);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jstring name = env->NewStringUTF(dotted_name);
        if (name) {
            result = static_cast<jclass>(env->CallStaticObjectMethod(class_class, for_name, name, JNI_FALSE, loader));
            if (env->ExceptionCheck()) { env->ExceptionClear(); result = nullptr; }
            env->DeleteLocalRef(name);
        }
        if (loader) env->DeleteLocalRef(loader);
    }
    env->DeleteLocalRef(class_class);
    return result;
}

// Idempotent; pumped from tick() while the override is on, on a 1 s cadence,
// until it takes. Every outcome is logged to dt.log under [worldtweaks] so
// a silent miss is one grep away.
void install_time_hook(JNIEnv* env) {
    if (g_timehook_installed.load(std::memory_order_acquire)) return;
    if (!env || !C.mc_class) return;
    const unsigned long long now = GetTickCount64();
    if (now < g_timehook_next_try_ms) return;
    g_timehook_next_try_ms = now + 1000;

    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) return;
    const jnihook_result_t ir = JNIHook_Init(vm);
    if (ir != JNIHOOK_OK && ir != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        VLOG_WARN("worldtweaks", "time hook: JNIHook_Init failed r={} — the sky may flash once a second", (int)ir);
        wlog("time hook: JNIHook_Init failed r=%d", (int)ir);
        g_timehook_next_try_ms = now + 30000;
        return;
    }

    if (!g_timepkt_hook_class) {
        jclass k = find_class_beside(env, C.mc_class, "net.minecraft.class_2761");
        if (!k) {
            if (!g_timehook_logged_wait) {
                g_timehook_logged_wait = true;
                VLOG_INFO("worldtweaks", "time hook: WorldTimeUpdateS2CPacket not resolvable yet; retrying");
                wlog("time hook: class_2761 not resolvable yet; retrying");
            }
            return;
        }
        g_timepkt_hook_class = static_cast<jclass>(env->NewGlobalRef(k));
        env->DeleteLocalRef(k);
        if (!g_timepkt_hook_class) return;
    }

    const char* name = "comp_3220";                       // 1.21.2+ record accessor
    jmethodID mid = env->GetMethodID(g_timepkt_hook_class, name, "()J");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!mid) {
        name = "method_11873";                            // 1.21 / 1.21.1 getTimeOfDay
        mid = env->GetMethodID(g_timepkt_hook_class, name, "()J");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!mid) {
        VLOG_WARN("worldtweaks", "time hook: neither comp_3220 nor method_11873 on class_2761 — unsupported version");
        wlog("time hook: timeOfDay accessor not found");
        g_timehook_next_try_ms = now + 60000;
        return;
    }

    const int ar = attach_time_hook_seh(mid);
    if (ar != JNIHOOK_OK) {
        ORIG_pkt_timeOfDay = nullptr;
        VLOG_WARN("worldtweaks", "time hook: JNIHook_Attach({}) failed r={}", name, ar);
        wlog("time hook: JNIHook_Attach failed r=%d", ar);
        g_timehook_next_try_ms = now + 30000;
        return;
    }
    g_timehook_installed.store(true, std::memory_order_release);
    VLOG_INFO("worldtweaks", "time hook: WorldTimeUpdateS2CPacket.{}() detour live — server time no longer reaches the sky while Time is on", name);
    wlog("time hook: WorldTimeUpdateS2CPacket.%s() detour LIVE", name);
}

void log_resolution_once() {
    if (g_logged_resolution) return;
    g_logged_resolution = true;
    wlog("resolution: mc=%d getInstance=%d worldFld=%d playerFld=%d | worldBase=%d propsFld=%d | "
         "clientWorld=%d setTime=%d | entX=%d entY=%d entZ=%d",
         C.mc_class != nullptr, C.mc_getInstance != nullptr, C.mc_world != nullptr, C.mc_player != nullptr,
         C.world_base != nullptr, C.fld_properties != nullptr,
         C.world_client != nullptr, C.m_setTime != nullptr,
         C.ent_getX != nullptr, C.ent_getY != nullptr, C.ent_getZ != nullptr);
}

} // namespace

bool wants_cancel_time_packet(void* env_opaque, void* packet_opaque) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!g_settings.time_enabled) return false;
    JNIEnv* env    = static_cast<JNIEnv*>(env_opaque);
    jobject packet = static_cast<jobject>(packet_opaque);
    if (!env || !packet) return false;
    g_pkt_seen.fetch_add(1, std::memory_order_relaxed);   // proves the listener fires

    // Fast path: once we've captured the packets' own WorldTimeUpdate class,
    // IsInstanceOf is same-classloader and reliable.
    if (g_timeupdate_class) {
        const bool is = env->IsInstanceOf(packet, g_timeupdate_class) == JNI_TRUE;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (is) g_timepkt_cancelled.fetch_add(1, std::memory_order_relaxed);
        return is;
    }

    // Discovery: match by class name (classloader-agnostic). The runtime name is
    // dotted, e.g. "net.minecraft.class_2761"; compare the final segment exactly
    // so class_27610+ can't false-match.
    jclass pc = env->GetObjectClass(packet);
    if (!pc) return false;
    if (!g_class_getName) {
        jclass cc = env->GetObjectClass(pc);   // java.lang.Class
        if (cc) {
            g_class_getName = env->GetMethodID(cc, "getName", "()Ljava/lang/String;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cc);
        }
    }
    bool matched = false;
    if (g_class_getName) {
        jstring nm = static_cast<jstring>(env->CallObjectMethod(pc, g_class_getName));
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (nm) {
            const char* s = env->GetStringUTFChars(nm, nullptr);
            if (s) {
                if (g_settings.debug && g_names_dumped < 80) { ++g_names_dumped; wlog("pkt seen: %s", s); }
                const char* dot = std::strrchr(s, '.');
                const char* base = dot ? dot + 1 : s;
                if (std::strcmp(base, "class_2761") == 0) {
                    matched = true;
                    if (!g_timepkt_logged) { g_timepkt_logged = true; wlog("time packet class discovered: %s", s); }
                }
                env->ReleaseStringUTFChars(nm, s);
            }
            env->DeleteLocalRef(nm);
        }
    }
    if (matched) {
        g_timeupdate_class = static_cast<jclass>(env->NewGlobalRef(pc));
        g_timepkt_cancelled.fetch_add(1, std::memory_order_relaxed);
    }
    env->DeleteLocalRef(pc);
    return matched;
}

void install() { wlog("install()"); }
void shutdown() {}

void on_disabled() {
    time_seeded = false;
    g_override_tod.store(-1, std::memory_order_relaxed);
    last_slider = INT32_MIN;
    particle_accum = 0.f;
    { std::lock_guard<std::mutex> lk(g_pool_mx); g_pool.clear(); }
    g_logged_resolution = false;
    g_last_status_ms = 0;
    wlog("on_disabled(): state reset");
}

void tick(void* env_opaque, float dt_ms) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    JNIEnv* env = static_cast<JNIEnv*>(env_opaque);
    if (!env) return;
    if (dt_ms < 0.f) dt_ms = 0.f;
    if (dt_ms > 250.f) dt_ms = 250.f;

    resolve_all(env);
    if (g_settings.debug) log_resolution_once();
    if (!C.mc_class || !C.mc_getInstance || !C.mc_world) return;

    // Arm the packet detour the first time the override is on (see the note
    // above install_time_hook). Only then: users who never touch Time never pay
    // for a class retransform.
    if (g_settings.time_enabled) install_time_hook(env);

    jobject mc = get_client(env);
    if (!mc) return;

    jobject world  = env->GetObjectField(mc, C.mc_world);
    jobject player = C.mc_player ? env->GetObjectField(mc, C.mc_player) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    // ── time ──
    if (world && C.world_base && C.fld_properties) {
        jobject props = env->GetObjectField(world, C.fld_properties);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (props) { apply_time(env, world, props, dt_ms); env->DeleteLocalRef(props); }
    }

    // ── particles: spawn + simulate around the player ──
    bool have_player = false; double px = 0, py = 0, pz = 0;
    if (player && C.ent_getX && C.ent_getY && C.ent_getZ) {
        px = env->CallDoubleMethod(player, C.ent_getX);
        py = env->CallDoubleMethod(player, C.ent_getY);
        pz = env->CallDoubleMethod(player, C.ent_getZ);
        if (env->ExceptionCheck()) env->ExceptionClear(); else have_player = true;
    }
    if (have_player)
        simulate_particles((float)px, (float)py, (float)pz, dt_ms);

    if (g_settings.debug) {
        const unsigned long long now = GetTickCount64();
        if (now - g_last_status_ms >= 1000) {
            g_last_status_ms = now;
            size_t alive = 0; { std::lock_guard<std::mutex> lk(g_pool_mx); alive = g_pool.size(); }
            const int cancels = g_timepkt_cancelled.exchange(0, std::memory_order_relaxed);
            const int pkts    = g_pkt_seen.exchange(0, std::memory_order_relaxed);
            wlog("status: world=%d player=%d setTime=%d timePktClass=%d timeHook=%d | time_on=%d slider=%d speed=%.1f disp_time=%.0f | "
                 "pkts_seen_last_s=%d timeCancels_last_s=%d extChanges_last_s=%d | ambient=%d dens=%d radius=%d alive=%zu spawned_last_s=%d",
                 world != nullptr, (int)have_player, C.m_setTime != nullptr, g_timeupdate_class != nullptr,
                 (int)g_timehook_installed.load(std::memory_order_relaxed),
                 (int)g_settings.time_enabled, g_settings.time_ticks, g_settings.time_speed, disp_time,
                 pkts, cancels, g_ext_changes,
                 g_settings.ambient_mode, g_settings.ambient_density, g_settings.ambient_radius,
                 alive, g_stat_spawned);
            g_stat_spawned = 0;
            g_ext_changes = 0;
        }
    }

    if (player) env->DeleteLocalRef(player);
    if (world)  env->DeleteLocalRef(world);
    env->DeleteLocalRef(mc);
}

void render_text(int display_w, int display_h) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (display_w < 2 || display_h < 2) return;
    if ((AmbientMode)g_settings.ambient_mode == AmbientMode::Off) return;

    // Arm the shared camera capture every frame (counter + pump), same as blockesp.
    hf::render_frame f{};
    {
        ::dt::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();
        if (!cap::is_installed()) {
            static double s_next = 0.0;
            const double now = ImGui::GetTime();
            if (now >= s_next) { s_next = now + 1.0; cap::install(); }
        }
        cap::set_capture_enabled(true);
        cap::capture_now(env);
        if (!hf::latest_frame(f) || !f.valid) return;
        // The capture carries the camera rotation only; fold in the walking
        // bob and hurt tilt the world was drawn with, or the particles swim
        // against the terrain with every step (see world_overlay_view.h).
        overlay_view::apply_view_bob(env, f);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    WorldOverlayDrawScope draw_scope(dl);
    // Replace the inherited canvas clip rect with the real framebuffer (the same
    // scissor fix BlockESP needed — see blockesp.cpp render_text).
    dl->PushClipRect(ImVec2(0.f, 0.f), ImVec2((float)display_w, (float)display_h), false);

    // Perspective scale: projection m11 = 1/tan(fovy/2); screen px per world unit
    // at distance d is m11 * (viewport_h/2) / d.
    const float fy = (f.projection[5] != 0.f) ? f.projection[5] : 1.f;
    const float half_h = 0.5f * (float)display_h;
    const float now_t = (float)ImGui::GetTime();

    // Snapshot base colour once.
    const float bcr = g_settings.col_r, bcg = g_settings.col_g, bcb = g_settings.col_b;
    const float szMul = g_settings.size;
    const bool  rainbow = g_settings.rainbow;

    std::lock_guard<std::mutex> lk(g_pool_mx);
    for (const CParticle& p : g_pool) {
        const AmbientMode m = (AmbientMode)p.kind;

        float sx, sy, depth;
        if (!hf::world_to_screen(f, p.x, p.y, p.z, sx, sy, depth, /*clip_z=*/false))
            continue;
        if (sx < -64.f || sy < -64.f || sx > display_w + 64.f || sy > display_h + 64.f)
            continue;

        const double ddx = p.x - f.cam_x, ddy = p.y - f.cam_y, ddz = p.z - f.cam_z;
        const float dist = (float)std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        if (dist < 0.05f) continue;

        // World size per preset, projected to pixels.
        float world_r;
        switch (m) {
            case AmbientMode::Fireflies: world_r = 0.10f; break;
            case AmbientMode::Embers:    world_r = 0.08f; break;
            case AmbientMode::Snow:      world_r = 0.09f; break;
            case AmbientMode::Sparkles:  world_r = 0.07f; break;
            case AmbientMode::Magic:     world_r = 0.10f; break;
            case AmbientMode::Souls:     world_r = 0.11f; break;
            default:                     world_r = 0.09f; break;
        }
        float R = world_r * p.sizeMul * szMul * fy * half_h / dist;
        if (R < 1.2f) R = 1.2f;
        if (R > 64.f) R = 64.f;

        // Life fade (in over first 15%, out over last 30%).
        const float t = p.age / p.life;
        float fade = 1.f;
        if (t < 0.15f) fade = t / 0.15f;
        else if (t > 0.70f) fade = (1.f - t) / 0.30f;
        if (fade < 0.f) fade = 0.f; if (fade > 1.f) fade = 1.f;

        // Twinkle per preset.
        float tw = 1.f;
        switch (m) {
            case AmbientMode::Fireflies: tw = 0.55f + 0.45f * std::sin(p.age * 2.2f + p.seed * 6.28f); break;
            case AmbientMode::Embers:    tw = 0.60f + 0.40f * std::sin(p.age * 9.0f + p.seed * 6.28f); break;
            case AmbientMode::Snow:      tw = 0.85f + 0.15f * std::sin(p.age * 0.6f + p.seed * 6.28f); break;
            case AmbientMode::Sparkles: { float s = std::sin(p.age * 6.0f + p.seed * 6.28f); tw = 0.25f + 0.75f * (s * s * s * s); break; }
            case AmbientMode::Magic:     tw = 0.70f + 0.30f * std::sin(p.age * 1.6f + p.seed * 6.28f); break;
            case AmbientMode::Souls:     tw = 0.65f + 0.35f * std::sin(p.age * 1.3f + p.seed * 6.28f); break;
            default: break;
        }

        float alpha = fade * tw;
        if (alpha <= 0.02f) continue;

        // Colour: rainbow hue-cycle, or the user's base colour.
        float cr = bcr, cg = bcg, cb = bcb;
        if (rainbow) {
            const float hue = now_t * 0.12f + p.seed;
            hsv2rgb(hue, 0.85f, 1.0f, cr, cg, cb);
        }
        // Embers darken/redshift toward the end of life.
        if (m == AmbientMode::Embers) {
            const float k = 1.f - t * 0.5f;
            cr *= 1.0f; cg *= (0.55f + 0.45f * k); cb *= (0.25f + 0.35f * k);
        }

        // Hot core: blend the colour toward white for a bright centre.
        const float hr = cr + (1.f - cr) * 0.65f;
        const float hg = cg + (1.f - cg) * 0.65f;
        const float hb = cb + (1.f - cb) * 0.65f;

        const ImVec2 c(sx, sy);
        constexpr float k60 = 1.04719755f;   // 60° in radians
        switch (m) {
            case AmbientMode::Fireflies: {
                // C · twin with a motion trail: a dimmer glow behind the velocity.
                const float vlen = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
                if (vlen > 0.01f) {
                    const float inv = 0.35f / vlen;
                    float tsx, tsy, td;
                    if (hf::world_to_screen(f, p.x - p.vx * inv, p.y - p.vy * inv, p.z - p.vz * inv,
                                            tsx, tsy, td, false))
                        soft_glow(dl, ImVec2(tsx, tsy), R * 1.5f, R * 1.5f, cr, cg, cb, alpha * 0.30f);
                }
                soft_glow(dl, c, R * 2.4f, R * 2.4f, cr, cg, cb, alpha * 0.38f);
                soft_glow(dl, c, R * 1.2f, R * 1.2f, cr, cg, cb, alpha * 0.75f);
                soft_glow(dl, c, R * 0.55f, R * 0.55f, hr, hg, hb, alpha);
                break;
            }
            case AmbientMode::Embers: {
                // A · smooth flame teardrop: a column of soft glows, round + wide
                // at the base and shrinking as it rises to a point. All circles →
                // no straight edges (the old single triangle looked "square").
                soft_glow(dl, ImVec2(sx, sy + R * 0.55f), R * 1.25f, R * 1.25f, cr, cg, cb, alpha * 0.42f);
                soft_glow(dl, ImVec2(sx, sy - R * 0.05f), R * 1.00f, R * 1.20f, cr, cg, cb, alpha * 0.56f);
                soft_glow(dl, ImVec2(sx, sy - R * 0.85f), R * 0.72f, R * 1.05f, cr, cg, cb, alpha * 0.66f);
                soft_glow(dl, ImVec2(sx, sy - R * 1.65f), R * 0.46f, R * 0.90f, cr, cg, cb, alpha * 0.68f);
                soft_glow(dl, ImVec2(sx, sy - R * 2.35f), R * 0.26f, R * 0.60f, cr, cg, cb, alpha * 0.55f);
                soft_glow(dl, ImVec2(sx, sy + R * 0.35f), R * 0.55f, R * 0.70f, hr, hg, hb, alpha); // hot core low
                break;
            }
            case AmbientMode::Snow: {
                // A · true 6-fold dendrite: crisp arms with ±60° branches.
                const float rot = p.seed * 6.28f + p.age * 0.3f;
                const float arm = R * 2.7f;
                float lw = R * 0.16f; if (lw < 1.1f) lw = 1.1f;
                const ImU32 sc = rgba_u32(hr, hg, hb, alpha * 0.9f);
                soft_glow(dl, c, R * 1.2f, R * 1.2f, cr, cg, cb, alpha * 0.30f);
                for (int k = 0; k < 6; ++k) {
                    const float a  = rot + (float)k * k60;
                    const float dx = std::cos(a), dy = std::sin(a);
                    dl->AddLine(c, ImVec2(sx + dx * arm, sy + dy * arm), sc, lw);
                    for (int bi = 0; bi < 2; ++bi) {
                        const float tpos = bi == 0 ? 0.5f : 0.72f;
                        const float bl   = arm * (bi == 0 ? 0.34f : 0.24f);
                        const ImVec2 bp(sx + dx * arm * tpos, sy + dy * arm * tpos);
                        dl->AddLine(bp, ImVec2(bp.x + std::cos(a + k60) * bl, bp.y + std::sin(a + k60) * bl), sc, lw * 0.85f);
                        dl->AddLine(bp, ImVec2(bp.x + std::cos(a - k60) * bl, bp.y + std::sin(a - k60) * bl), sc, lw * 0.85f);
                    }
                }
                soft_glow(dl, c, R * 0.45f, R * 0.45f, hr, hg, hb, alpha * 0.9f);
                break;
            }
            case AmbientMode::Sparkles: {
                // B · 8-point burst: 4 long axes + 4 shorter diagonals.
                const float rot = p.age * 1.2f + p.seed * 6.28f;
                soft_glow(dl, c, R * 1.6f, R * 1.6f, cr, cg, cb, alpha * 0.40f);
                star(dl, c, 4, R * 3.7f, R * 0.26f, rot, hr, hg, hb, alpha);
                star(dl, c, 4, R * 2.0f, R * 0.22f, rot + 0.785398f, cr, cg, cb, alpha * 0.7f);
                soft_glow(dl, c, R * 0.5f, R * 0.5f, hr, hg, hb, alpha);
                break;
            }
            case AmbientMode::Magic: {
                // New take · arcane core with small motes orbiting an ellipse.
                soft_glow(dl, c, R * 2.3f, R * 2.3f, cr, cg, cb, alpha * 0.40f);
                soft_glow(dl, c, R * 1.0f, R * 1.0f, cr, cg, cb, alpha * 0.75f);
                soft_glow(dl, c, R * 0.45f, R * 0.45f, hr, hg, hb, alpha);
                for (int k = 0; k < 3; ++k) {
                    const float a = p.age * 2.2f + (float)k * 2.0944f + p.seed * 6.28f;
                    const ImVec2 mote(sx + std::cos(a) * R * 2.3f, sy + std::sin(a) * R * 1.4f);
                    soft_glow(dl, mote, R * 0.55f, R * 0.55f, hr, hg, hb, alpha * 0.85f);
                }
                break;
            }
            case AmbientMode::Souls: {
                // C · ghost: tall soft body with two faint side wisps.
                const ImVec2 up(sx, sy - R * 0.6f);
                soft_glow(dl, c, R * 1.4f, R * 2.9f, cr, cg, cb, alpha * 0.33f);
                soft_glow(dl, ImVec2(sx - R * 1.0f, sy - R * 0.2f), R * 0.5f, R * 1.0f, cr, cg, cb, alpha * 0.38f);
                soft_glow(dl, ImVec2(sx + R * 1.1f, sy),            R * 0.45f, R * 0.9f, cr, cg, cb, alpha * 0.32f);
                soft_glow(dl, up, R * 0.45f, R * 0.85f, hr, hg, hb, alpha * 0.9f);
                break;
            }
            default:
                soft_glow(dl, c, R * 2.0f, R * 2.0f, cr, cg, cb, alpha * 0.5f);
                soft_glow(dl, c, R * 0.6f, R * 0.6f, hr, hg, hb, alpha);
                break;
        }
    }

    dl->PopClipRect();
}

} // namespace dt::modules::worldtweaks


