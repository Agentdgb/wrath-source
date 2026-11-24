// hitfx.cpp — comic hit-effects engine. See hitfx.h.

#include "hitfx.h"
#include "hitfx_render_context.h"
#include "hitfx_capture_hook.h"
#include "hitfx_debug.h"
#include "comic_font.h"

#include "modules/render/arraylist/font_pool.h"
#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include "util/Log.h"
#include "vgui/bridge.h"     // isOverlayVisible() — stand down while the panel is open

#include "imgui.h"

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace vanta::modules::hitfx {

bool     g_enabled = false;
settings g_settings{};
std::function<void(settings&)> g_settings_provider{};

namespace {

// ── Verified Yarn 1.21.11 symbols ────────────────────────────────────────────
constexpr const char* kMcClass       = "net/minecraft/class_310";
constexpr const char* kWorldClass    = "net/minecraft/class_638";
constexpr const char* kPlayerClass   = "net/minecraft/class_1657";
constexpr const char* kLivingClass   = "net/minecraft/class_1309";
constexpr const char* kEntityClass   = "net/minecraft/class_1297";

// ── JNI id cache ─────────────────────────────────────────────────────────────
struct jni_cache {
    bool tried = false, ok = false;
    jclass    mc_class = nullptr, living_class = nullptr, player_class = nullptr;
    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_world_fid = nullptr, mc_player_fid = nullptr;
    jmethodID world_getEntities = nullptr;
    jmethodID iterable_iterator = nullptr, iterator_hasNext = nullptr, iterator_next = nullptr;
    jmethodID ent_getX = nullptr, ent_getY = nullptr, ent_getZ = nullptr;
    jmethodID ent_isAlive = nullptr, ent_getUuidStr = nullptr;
    jfieldID  living_hurtTime = nullptr, living_maxHurt = nullptr;
};
jni_cache C{};

bool init_cache(JNIEnv* env)
{
    if (C.tried) return C.ok;
    C.tried = true;

    jclass mc     = vanta::jvm::findClassAnyLoader(env, kMcClass);
    jclass world  = vanta::jvm::findClassAnyLoader(env, kWorldClass);
    jclass player = vanta::jvm::findClassAnyLoader(env, kPlayerClass);
    jclass living = vanta::jvm::findClassAnyLoader(env, kLivingClass);
    jclass entity = vanta::jvm::findClassAnyLoader(env, kEntityClass);
    if (!mc || !world || !player || !living || !entity) {
        VLOG_WARN("hitfx", "poll: core classes unresolved");
        return false;
    }

    C.mc_getInstance = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    C.mc_world_fid   = env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;");
    C.mc_player_fid  = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
    C.world_getEntities = env->GetMethodID(world, "method_18112", "()Ljava/lang/Iterable;");

    C.ent_getX = env->GetMethodID(entity, "method_23317", "()D");
    C.ent_getY = env->GetMethodID(entity, "method_23318", "()D");
    C.ent_getZ = env->GetMethodID(entity, "method_23321", "()D");
    C.ent_isAlive    = env->GetMethodID(entity, "method_5805", "()Z");
    C.ent_getUuidStr = env->GetMethodID(entity, "method_5845", "()Ljava/lang/String;");
    C.living_hurtTime = env->GetFieldID(living, "field_6235", "I");
    C.living_maxHurt  = env->GetFieldID(living, "field_6254", "I");

    // Generic java.lang.Iterable / java.util.Iterator (not obfuscated).
    jclass iterable = env->FindClass("java/lang/Iterable");
    jclass iterator = env->FindClass("java/util/Iterator");
    if (iterable) C.iterable_iterator = env->GetMethodID(iterable, "iterator", "()Ljava/util/Iterator;");
    if (iterator) {
        C.iterator_hasNext = env->GetMethodID(iterator, "hasNext", "()Z");
        C.iterator_next    = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    C.living_class  = static_cast<jclass>(env->NewGlobalRef(living));
    C.player_class  = static_cast<jclass>(env->NewGlobalRef(player));
    C.mc_class      = static_cast<jclass>(env->NewGlobalRef(mc));

    env->DeleteLocalRef(mc); env->DeleteLocalRef(world); env->DeleteLocalRef(player);
    env->DeleteLocalRef(living); env->DeleteLocalRef(entity);
    if (iterable) env->DeleteLocalRef(iterable);
    if (iterator) env->DeleteLocalRef(iterator);

    C.ok = C.mc_getInstance && C.mc_world_fid && C.mc_player_fid && C.world_getEntities
        && C.ent_getX && C.ent_getY && C.ent_getZ && C.ent_isAlive && C.ent_getUuidStr
        && C.living_hurtTime && C.living_maxHurt
        && C.iterable_iterator && C.iterator_hasNext && C.iterator_next
        && C.living_class && C.player_class;
    dbg::log("poll: init_cache ok=%d [getInstance=%p world_fid=%p player_fid=%p getEntities=%p "
             "getX=%p isAlive=%p uuidStr=%p hurtTime=%p maxHurt=%p iter=%p]",
             (int)C.ok, (void*)C.mc_getInstance, (void*)C.mc_world_fid, (void*)C.mc_player_fid,
             (void*)C.world_getEntities, (void*)C.ent_getX, (void*)C.ent_isAlive,
             (void*)C.ent_getUuidStr, (void*)C.living_hurtTime, (void*)C.living_maxHurt,
             (void*)C.iterable_iterator);
    if (!C.ok) VLOG_WARN("hitfx", "poll: some entity members unresolved — hit detection off");
    return C.ok;
}

// ── Particle pool ────────────────────────────────────────────────────────────
struct Particle {
    double wx, wy, wz;
    double vx, vy, vz;
    float  ageMs = 0.f, lifeMs = 1000.f;
    float  rot = 0.f;
    float  baseScale = 1.f;
    const char* word = "POW";        // points to a static literal (stable)
    float  cr = 1.f, cg = 1.f, cb = 1.f;   // base RGB colour (alpha applied at draw)
    bool   anime = false;            // which font atlas to render with
    bool   paw   = false;            // true = draw a cat paw print instead of a word
};
std::mutex             g_pool_mx;
std::vector<Particle>  g_pool;
std::unordered_map<std::string, int> g_prevHurt;

// Classic comic hit words with the "!" (Breeze renders e.g. "BIFF!").
const char* COMIC_WORDS[] = {
    "POW!", "BAM!", "BOOM!", "WHAM!", "ZAP!", "BIFF!", "SMACK!", "KAPOW!",
    "CRASH!", "BANG!", "ZONK!", "WHACK!", "THWACK!", "SPLAT!", "SOCK!", "OOF!"
};
constexpr int COMIC_COUNT = sizeof(COMIC_WORDS) / sizeof(COMIC_WORDS[0]);

// Anime / kawaii words.
const char* ANIME_WORDS[] = { "uwu", "owo", "7w7", "onichan", "meow", "nya~", "senpai", "baka", "kawaii", ">w<", "OwO", "^w^", "rawr" };
constexpr int ANIME_COUNT = sizeof(ANIME_WORDS) / sizeof(ANIME_WORDS[0]);

// Cute pastel palette for anime mode (RGB 0..1).
struct rgb3 { float r, g, b; };
const rgb3 ANIME_COLORS[] = {
    { 1.00f, 0.58f, 0.79f },  // sakura pink
    { 0.78f, 0.60f, 1.00f },  // lavender
    { 0.60f, 0.86f, 1.00f },  // sky blue
    { 0.70f, 1.00f, 0.85f },  // mint
    { 1.00f, 0.78f, 0.62f },  // peach
    { 1.00f, 0.62f, 0.90f },  // hot pink
    { 0.85f, 0.75f, 1.00f },  // periwinkle
};
constexpr int ANIME_COLOR_COUNT = sizeof(ANIME_COLORS) / sizeof(ANIME_COLORS[0]);

std::mt19937& rng()
{
    static std::mt19937 g{ std::random_device{}() };
    return g;
}
float frand(float a, float b) { std::uniform_real_distribution<float> d(a, b); return d(rng()); }
int   irand(int a, int b)     { std::uniform_int_distribution<int> d(a, b);     return d(rng()); }

inline ImU32 pack_rgba(float r, float g, float b, float a)
{
    auto b8 = [](float x){ x = std::clamp(x, 0.f, 1.f); return (int)(x * 255.f + 0.5f); };
    return IM_COL32(b8(r), b8(g), b8(b), b8(a));
}

// Draw one comic word as a ROTATED billboard (Breeze rotates each word ±20°).
// ImGui AddText is axis-aligned, so we emit the glyph quads ourselves, rotating
// each corner around the word centre (cx,cy). The comic atlas texture must be
// pushed on `dl` before calling this.
void draw_word_rotated(ImDrawList* dl, ImFont* font, float px_size,
                       const char* text, float cx, float cy, float rot, ImU32 col)
{
    if (!font || font->FontSize <= 0.f) return;
    const float scale = px_size / font->FontSize;

    // Measure advance width + vertical extent (at render scale).
    float w = 0.f, min_y = 1e9f, max_y = -1e9f;
    for (const char* s = text; *s; ++s) {
        const ImFontGlyph* g = font->FindGlyph((ImWchar)(unsigned char)*s);
        if (!g) continue;
        w += g->AdvanceX * scale;
        min_y = std::min(min_y, g->Y0 * scale);
        max_y = std::max(max_y, g->Y1 * scale);
    }
    if (max_y < min_y) { min_y = 0.f; max_y = px_size; }

    const float x_start = -w * 0.5f;                 // centre horizontally
    const float y_off   = -(min_y + max_y) * 0.5f;   // centre on the visual mid
    const float cr = std::cos(rot), sr = std::sin(rot);
    auto R = [&](float lx, float ly) { return ImVec2(cx + lx * cr - ly * sr, cy + lx * sr + ly * cr); };

    float pen = x_start;
    for (const char* s = text; *s; ++s) {
        const ImFontGlyph* g = font->FindGlyph((ImWchar)(unsigned char)*s);
        if (!g) continue;
        if (g->Visible) {
            const float x0 = pen + g->X0 * scale, x1 = pen + g->X1 * scale;
            const float y0 = y_off + g->Y0 * scale, y1 = y_off + g->Y1 * scale;
            dl->PrimReserve(6, 4);
            dl->PrimQuadUV(R(x0, y0), R(x1, y0), R(x1, y1), R(x0, y1),
                           ImVec2(g->U0, g->V0), ImVec2(g->U1, g->V0),
                           ImVec2(g->U1, g->V1), ImVec2(g->U0, g->V1), col);
        }
        pen += g->AdvanceX * scale;
    }
}

// Draw a cute cat paw print (one big pad + four toe beans) as a filled shape,
// rotated to match the particle. Filled circles sample ImGui's white pixel, so
// this must be called with the glyph atlas texture NOT pushed (default atlas).
void draw_paw(ImDrawList* dl, float cx, float cy, float size, float rot, ImU32 col)
{
    const float cr = std::cos(rot), sr = std::sin(rot);
    auto R = [&](float lx, float ly) { return ImVec2(cx + lx * cr - ly * sr, cy + lx * sr + ly * cr); };

    // Main pad, sitting a little below centre.
    dl->AddCircleFilled(R(0.f, size * 0.16f), size * 0.42f, col, 28);

    // Four toe beans arching across the top.
    static const float toes[4][3] = {
        { -0.34f, -0.30f, 0.185f }, { -0.12f, -0.46f, 0.205f },
        {  0.12f, -0.46f, 0.205f }, {  0.34f, -0.30f, 0.185f },
    };
    for (auto& t : toes)
        dl->AddCircleFilled(R(t[0] * size, t[1] * size), t[2] * size, col, 20);
}

std::string read_jstr(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* p = env->GetStringUTFChars(js, nullptr);
    std::string s = p ? p : "";
    if (p) env->ReleaseStringUTFChars(js, p);
    return s;
}

// Spawn 1-3 particles for a freshly-hit entity at (ex,ey,ez), player at (px,py,pz).
void spawn_burst(double ex, double ey, double ez, double px, double py, double pz)
{
    const float dist = (float)std::sqrt((ex-px)*(ex-px) + (ey-py)*(ey-py) + (ez-pz)*(ez-pz));
    const double amount4 = std::max(2.0f, dist) / 10.0;     // Breeze's distance factor
    const float  burst   = std::max(2.0f, dist / 2.0f);     // Breeze's velocity scale
    const float  height  = 1.8f;                            // player standing height

    const bool anime = (g_settings.which == mode::Anime);

    // Common burst physics + colour, shared by word and paw particles.
    auto make = [&]() -> Particle {
        Particle p{};
        p.wx = ex + frand(-0.15f, 0.30f);
        p.wy = ey + frand(0.40f, height);
        p.wz = ez + frand(-0.15f, 0.15f);
        p.vx = (frand(0.f, 1.f) - 0.5f) * 2.0f * burst;
        p.vy = frand(0.f, 1.f) * 0.1f * burst;
        p.vz = (frand(0.f, 1.f) - 0.5f) * 2.0f * burst;
        p.rot = frand(-20.f, 20.f) * 3.14159265f / 180.f;
        p.baseScale = (float)(0.55f * std::max(0.5, 1.0 - amount4) + frand(0.f, 1.f) * 0.2f);
        p.lifeMs = frand(800.f, 1200.f);
        p.anime = anime;

        if (g_settings.rainbow) {
            if (anime) {
                // Anime rainbow = the cute pastel palette (random pick).
                const rgb3 c = ANIME_COLORS[irand(0, ANIME_COLOR_COUNT - 1)];
                p.cr = c.r; p.cg = c.g; p.cb = c.b;
            } else {
                // Comic rainbow = full-saturation hue cycle (Breeze).
                float r, g, b;
                ImGui::ColorConvertHSVtoRGB(frand(0.f, 1.f), 1.f, 1.f, r, g, b);
                p.cr = r; p.cg = g; p.cb = b;
            }
        } else {
            // Rainbow off → the user's chosen Color drives BOTH modes (so the
            // ClickGui picker takes effect in Anime too), with Breeze's small
            // ±0.1 hue jitter for variety.
            float hh, ss, vv;
            ImGui::ColorConvertRGBtoHSV(g_settings.col_r, g_settings.col_g, g_settings.col_b, hh, ss, vv);
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hh + frand(-0.1f, 0.1f), ss, vv, r, g, b);
            p.cr = r; p.cg = g; p.cb = b;
        }
        return p;
    };

    std::lock_guard<std::mutex> lk(g_pool_mx);

    // Words.
    const int n = irand(1, 3);
    for (int i = 0; i < n; ++i) {
        Particle p = make();
        p.word = anime ? ANIME_WORDS[irand(0, ANIME_COUNT - 1)]
                       : COMIC_WORDS[irand(0, COMIC_COUNT - 1)];
        g_pool.push_back(p);
    }

    // Cat paw prints — anime mode only, a couple per burst.
    if (anime) {
        const int paws = irand(1, 2);
        for (int i = 0; i < paws; ++i) {
            Particle p = make();
            p.paw = true;
            p.baseScale *= 0.85f;   // paws a touch smaller than the words
            g_pool.push_back(p);
        }
    }
}

// ── Hit polling (nearby players' hurtTime rising edge) ───────────────────────
void poll_hits(JNIEnv* env)
{
    if (!init_cache(env)) return;

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;

    jobject world  = env->GetObjectField(mc, C.mc_world_fid);
    jobject player = env->GetObjectField(mc, C.mc_player_fid);
    if (!world || !player) {
        if (world)  env->DeleteLocalRef(world);
        if (player) env->DeleteLocalRef(player);
        env->DeleteLocalRef(mc);
        return;
    }

    const double px = env->CallDoubleMethod(player, C.ent_getX);
    const double py = env->CallDoubleMethod(player, C.ent_getY);
    const double pz = env->CallDoubleMethod(player, C.ent_getZ);

    jobject iterable = env->CallObjectMethod(world, C.world_getEntities);
    jobject it = iterable ? env->CallObjectMethod(iterable, C.iterable_iterator) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    std::unordered_map<std::string, int> seen;
    const float range2 = g_settings.range * g_settings.range;
    int n_players = 0, n_hits = 0, max_hurt_seen = 0;

    while (it && env->CallBooleanMethod(it, C.iterator_hasNext) == JNI_TRUE) {
        jobject ent = env->CallObjectMethod(it, C.iterator_next);
        if (!ent) continue;

        // Players only (Breeze), not self, alive, within range.
        if (env->IsInstanceOf(ent, C.player_class) != JNI_TRUE) { env->DeleteLocalRef(ent); continue; }
        if (env->IsSameObject(ent, player)) { env->DeleteLocalRef(ent); continue; }
        if (env->CallBooleanMethod(ent, C.ent_isAlive) != JNI_TRUE) { env->DeleteLocalRef(ent); continue; }

        const double ex = env->CallDoubleMethod(ent, C.ent_getX);
        const double ey = env->CallDoubleMethod(ent, C.ent_getY);
        const double ez = env->CallDoubleMethod(ent, C.ent_getZ);
        const double d2 = (ex-px)*(ex-px) + (ey-py)*(ey-py) + (ez-pz)*(ez-pz);
        if (d2 > range2) { env->DeleteLocalRef(ent); continue; }

        ++n_players;
        const int hurt = env->GetIntField(ent, C.living_hurtTime);
        if (hurt > max_hurt_seen) max_hurt_seen = hurt;
        jstring ujs = static_cast<jstring>(env->CallObjectMethod(ent, C.ent_getUuidStr));
        std::string uuid = read_jstr(env, ujs);
        if (ujs) env->DeleteLocalRef(ujs);

        int prev = 0;
        auto pit = g_prevHurt.find(uuid);
        if (pit != g_prevHurt.end()) prev = pit->second;

        // Rising edge = just took a hit (server set hurtTime to max this tick).
        if (hurt > prev) {
            ++n_hits;
            const int bursts = 1 + irand(0, 2);
            for (int k = 0; k < bursts; ++k)   // Breeze: 1..3 bursts per hit
                spawn_burst(ex, ey, ez, px, py, pz);
            dbg::log("poll: HIT player uuid=%.8s hurt=%d prev=%d -> spawned %d bursts at (%.1f,%.1f,%.1f)",
                     uuid.c_str(), hurt, prev, bursts, ex, ey, ez);
        }
        seen[uuid] = hurt;

        env->DeleteLocalRef(ent);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    g_prevHurt.swap(seen);   // forget entities that left range/view

    dbg::log_rl(2, 2000, "poll: players_in_range=%d hits_this_frame=%d max_hurtTime=%d (range=%.0f)",
                n_players, n_hits, max_hurt_seen, g_settings.range);

    if (it) env->DeleteLocalRef(it);
    if (iterable) env->DeleteLocalRef(iterable);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(player);
    env->DeleteLocalRef(mc);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

float now_ms_delta()
{
    static auto last = std::chrono::steady_clock::now();
    const auto n = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float, std::milli>(n - last).count();
    last = n;
    return std::clamp(dt, 0.f, 100.f);
}

} // namespace

void install()
{
    dbg::log("module: install() called — installing capture hook");
    bool ok = capture::install();
    capture::set_capture_enabled(g_enabled);
    dbg::log("module: install() done — capture installed=%d enabled=%d", (int)ok, (int)g_enabled);
}

void shutdown()
{
    capture::set_capture_enabled(false);
    std::lock_guard<std::mutex> lk(g_pool_mx);
    g_pool.clear();
    g_prevHurt.clear();
}

void render_text(int display_w, int display_h)
{
    // Pull live settings so ClickGui edits (mode, colour, scale, glow) apply
    // immediately. Mode changes also clear the pool so lingering comic words
    // don't overlap freshly-spawned anime ones.
    if (g_settings_provider) {
        const mode prev = g_settings.which;
        g_settings_provider(g_settings);
        if (g_settings.which != prev) {
            std::lock_guard<std::mutex> lk(g_pool_mx);
            g_pool.clear();
        }
    }

    capture::set_capture_enabled(g_enabled);
    if (!g_enabled || display_w < 2 || display_h < 2)
        return;

    // While the ClickGui overlay is open it owns the screen — don't draw hit
    // effects over the panel. Clear any in-flight particles so none flash back
    // when it closes, and skip capture/polling entirely this frame.
    if (::vanta::vgui::bridge::isOverlayVisible()) {
        std::lock_guard<std::mutex> lk(g_pool_mx);
        g_pool.clear();
        return;
    }

    // The capture hook needs WorldRenderer (class_761) loaded, which only
    // happens once a world is joined — so onInit's install() may have failed.
    // Retry from here (rate-limited) until it attaches.
    if (!capture::is_installed()) {
        static unsigned long long s_next = 0;
        const unsigned long long now = (unsigned long long)ImGui::GetTime() * 1000ull;
        if (now >= s_next) {
            s_next = now + 1000;
            dbg::log_rl(5, 4000, "render: capture not installed yet — retrying install()");
            capture::install();
            capture::set_capture_enabled(g_enabled);
        }
    }

    // Everything runs on the render (SwapBuffers) thread, which has a JNIEnv.
    {
        vanta::jvm::JniScope s;
        if (s) {
            capture::capture_now(s.get());   // read MC's real matrices this frame
            poll_hits(s.get());
        } else {
            dbg::log_rl(6, 3000, "render: JniScope failed — no env on render thread");
        }
    }

    render_frame cam{};
    const bool have_cam = latest_frame(cam);

    // Font pool for the current mode (comic = badaboom, anime = comfortaa).
    const bool anime_mode = (g_settings.which == mode::Anime);
    hit_font& hfont = anime_mode ? anime_pool() : comic_pool();
    const bool have_font = hfont.ensure();

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float dt = now_ms_delta();
    // Breeze scale: badaboom native 150 × amount16 (envelope·baseScale·userScale).
    // px_size = 150 · sc reproduces Breeze's on-screen size exactly.
    const float base_px = 150.f;

    // Paw prints are collected here and drawn in a SECOND pass after the glyph
    // atlas texture is popped — filled circles need ImGui's default white pixel,
    // not our font atlas.
    struct PawDraw { float x, y, size, rot, cr, cg, cb, alpha; };
    std::vector<PawDraw> paws;

    if (have_font) dl->PushTextureID(hfont.texture_id());
    dl->PushClipRect(ImVec2(0.f, 0.f), ImVec2((float)display_w, (float)display_h), false);

    std::lock_guard<std::mutex> lk(g_pool_mx);
    int drawn = 0, offscreen = 0;
    const std::size_t pool_n = g_pool.size();
    for (std::size_t i = 0; i < g_pool.size(); ) {
        Particle& p = g_pool[i];
        p.ageMs += dt;
        if (p.ageMs >= p.lifeMs) { g_pool[i] = g_pool.back(); g_pool.pop_back(); continue; }

        const float t = p.ageMs / p.lifeMs;
        const float eased = std::pow(t, 0.15f);

        // Advance the world position by the decelerating burst (Breeze).
        const double wx = p.wx + p.vx * eased;
        const double wy = p.wy + p.vy * eased;
        const double wz = p.wz + p.vz * eased;

        ++i;

        if (!have_cam) continue;
        float sx, sy, depth;
        if (!world_to_screen(cam, wx, wy, wz, sx, sy, depth)) { ++offscreen; continue; }
        ++drawn;

        // Scale envelope (Breeze): grow in, hold, shrink.
        float sc;
        if (t < 0.1f)      sc = t / 0.1f;
        else if (t < 0.3f) sc = 1.0f;
        else               sc = 1.0f - (t - 0.3f) / 0.7f * 0.4f;
        sc *= p.baseScale * g_settings.scale;
        if (sc <= 0.001f) continue;

        // Alpha envelope (Breeze): opaque then fade out after 60%.
        float alpha = (t < 0.6f) ? 1.0f : (1.0f - (t - 0.6f) / 0.4f);
        alpha = std::clamp(alpha, 0.f, 1.f) * g_settings.col_a;

        const ImU32 col = pack_rgba(p.cr, p.cg, p.cb, alpha);

        const float px_size = std::max(6.f, base_px * sc);

        // Cat paw prints: queue for the second pass (drawn with the default
        // atlas texture), never through the glyph atlas.
        if (p.paw) {
            paws.push_back({ sx, sy, px_size * 0.5f, p.rot, p.cr, p.cg, p.cb, alpha });
            continue;
        }

        const char* word = p.word;
        if (have_font) {
            ImFont* font = hfont.get(px_size);
            if (font) {
                if (!anime_mode) {
                    // COMIC: thick black outline (Breeze's "BIFF!" look). A
                    // scaled-up black copy behind the fill gives a clean
                    // uniform border; a ring pass fills the corners solid; a
                    // small down-right drop shadow adds depth.
                    if (g_settings.glow) {
                        const ImU32 black = pack_rgba(0.f, 0.f, 0.f, alpha);
                        // drop shadow
                        const float ds = px_size * 0.045f;
                        draw_word_rotated(dl, font, px_size, word, sx + ds, sy + ds, p.rot,
                                          pack_rgba(0.f, 0.f, 0.f, alpha * 0.5f));
                        // outline: 12-sample ring at the border radius
                        const float orad = px_size * 0.05f;
                        for (int k = 0; k < 12; ++k) {
                            const float a = k * (6.2831853f / 12.f);
                            draw_word_rotated(dl, font, px_size, word,
                                              sx + std::cos(a) * orad, sy + std::sin(a) * orad,
                                              p.rot, black);
                        }
                    }
                    draw_word_rotated(dl, font, px_size, word, sx, sy, p.rot, col);
                } else {
                    // ANIME: a SUBTLE soft halo + chunky, well-filled letters.
                    if (g_settings.glow) {
                        // Tight concentric copies → an understated glow (not the
                        // big bloom from before). Continuous, no broken edges.
                        static const float layers[4][2] = {
                            { 1.12f, 0.06f }, { 1.085f, 0.10f },
                            { 1.055f, 0.15f }, { 1.028f, 0.20f },
                        };
                        for (auto& L : layers) {
                            ImFont* gf = hfont.get(px_size * L[0]);
                            if (!gf) gf = font;
                            const ImU32 gcol = pack_rgba(p.cr, p.cg, p.cb, alpha * L[1]);
                            draw_word_rotated(dl, gf, px_size * L[0], word, sx, sy, p.rot, gcol);
                        }
                    }
                    // Faux-bold: two slightly dilated fill passes fatten the
                    // strokes so the words read solid and chunky, then crisp.
                    draw_word_rotated(dl, font, px_size * 1.035f, word, sx, sy, p.rot, col);
                    draw_word_rotated(dl, font, px_size * 1.018f, word, sx, sy, p.rot, col);
                    draw_word_rotated(dl, font, px_size,          word, sx, sy, p.rot, col);
                }
            }
        } else {
            // Fallback: axis-aligned proxima if the atlas failed to build.
            auto& fb = arraylist::shared_pool();
            if (fb.ensure()) {
                ImFont* font = fb.get(px_size);
                if (font) {
                    const ImVec2 tsz = font->CalcTextSizeA(px_size, FLT_MAX, -1.f, word);
                    dl->PushTextureID(fb.texture_id());
                    dl->AddText(font, px_size, ImVec2(sx - tsz.x * 0.5f, sy - tsz.y * 0.5f), col, word);
                    dl->PopTextureID();
                }
            }
        }
    }

    dl->PopClipRect();
    if (have_font) dl->PopTextureID();

    // Second pass: cat paw prints, drawn with ImGui's default atlas so the
    // filled circles sample the white pixel correctly. A faint larger copy
    // gives a soft edge; the solid copy is the fill.
    if (!paws.empty()) {
        dl->PushTextureID(ImGui::GetIO().Fonts->TexID);
        dl->PushClipRect(ImVec2(0.f, 0.f), ImVec2((float)display_w, (float)display_h), false);
        for (const auto& pw : paws) {
            const ImU32 soft  = pack_rgba(pw.cr, pw.cg, pw.cb, pw.alpha * 0.18f);
            const ImU32 solid = pack_rgba(pw.cr, pw.cg, pw.cb, pw.alpha);
            draw_paw(dl, pw.x, pw.y, pw.size * 1.16f, pw.rot, soft);
            draw_paw(dl, pw.x, pw.y, pw.size,         pw.rot, solid);
        }
        dl->PopClipRect();
        dl->PopTextureID();
    }

    if (pool_n > 0)
        dbg::log_rl(3, 1000, "render: pool=%zu have_cam=%d drawn=%d offscreen=%d",
                    pool_n, (int)have_cam, drawn, offscreen);
}

} // namespace vanta::modules::hitfx
