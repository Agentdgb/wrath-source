#include "util/TimeBudget.h"
// blockesp.cpp — the BlockESP engine.
//
// Architecture (see blockesp.h for the public contract):
//
//   Registry snapshot   : one-shot walk of Registries.BLOCK on world-load,
//                         published to the ClickGui block-picker via
//                         vgui::bridge::setBlockPickerProvider.
//   Live scanner        : rolling triple-loop over a radius around the
//                         player. Cheap-and-correct MVP — a chunk-section
//                         iterator would be faster but the intermediary
//                         names for ClientChunkManager / WorldChunk /
//                         ChunkSection drift enough across 1.21.x that a
//                         plain getBlockState(new BlockPos) is the more
//                         reliable path. Spread over ~40 ticks so each
//                         tick pays ~1-2 ms of JNI, not a lump.
//   Packet reveal cache : PacketReceiveEvent listener types-checks each
//                         incoming packet against BlockUpdateS2CPacket and,
//                         if it matches, folds (pos, blockId) into the same
//                         cache. Antix-ray leak path — a diamond ore
//                         revealed by a neighbour break lands here and
//                         stays.
//   Render              : world→screen 8 AABB corners per cached pos, 12
//                         AddLine calls on the ImGui foreground drawlist.
//                         Colour = the block's map colour (or the user's
//                         custom colour).
//
// JNI resolution is direct FindClass / GetMethodID, cached in globals — the
// same lightweight pattern animations_hook and chams_item use. The mapping
// registry only carries the classes that already exist there; the new ones
// (Block, BlockState, Registries, WorldChunk, ...) live here.

#include "modules/render/blockesp/blockesp.h"

#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "core/event/Priority.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "modules/render/hitfx/hitfx_capture_hook.h"
#include "modules/render/hitfx/hitfx_render_context.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <imgui.h>
#include "modules/render/world_overlay_draw_scope.h"
#include "modules/render/world_overlay_view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hf = vanta::modules::hitfx;
namespace cap = vanta::modules::hitfx::capture;

namespace vanta::modules::blockesp {

settings          g_settings;
std::mutex        g_settings_mx;
std::atomic<bool> g_enabled{ false };
// Gates the desktop debug log. No panel switch drives it any more (removed by
// request); flip it here when diagnosing. Off = no file, no overhead.
std::atomic<bool> g_debug{ false };

namespace {

// ── Dedicated desktop log ────────────────────────────────────────────────────
// Mirrors animations_hook — writes to %USERPROFILE%\Desktop\gl_render_trace.txt
// so the user can hand a diagnostic back without digging through the main log.
std::mutex        g_blog_mx;
FILE*             g_blog_fp = nullptr;
char              g_blog_path[512] = {};
bool              g_blog_tried = false;
unsigned long long g_blog_t0 = 0;

bool blog_try_open(const char* p) {
    FILE* f = nullptr;
    if (fopen_s(&f, p, "wb") == 0 && f) {
        g_blog_fp = f;
        strncpy_s(g_blog_path, sizeof(g_blog_path), p, _TRUNCATE);
        return true;
    }
    return false;
}
void blog_open_once() {
    if (g_blog_tried) return;
    g_blog_tried = true;
    char buf[512]; size_t needed = 0;
    if (getenv_s(&needed, buf, sizeof(buf), "USERPROFILE") == 0 && needed) {
        std::string p(buf); p += "\\gl_render_trace.txt";
        blog_try_open(p.c_str());
    }
    if (!g_blog_fp && getenv_s(&needed, buf, sizeof(buf), "USERPROFILE") == 0 && needed) {
        std::string p(buf); p += "\\gl_render_trace.txt";
        blog_try_open(p.c_str());
    }
    if (!g_blog_fp && getenv_s(&needed, buf, sizeof(buf), "TEMP") == 0 && needed) {
        std::string p(buf); p += "\\gl_render_trace.txt";
        blog_try_open(p.c_str());
    }
    if (g_blog_fp) {
        g_blog_t0 = GetTickCount64();
        std::fprintf(g_blog_fp,
            "=== gl_render_trace debug log — session start ===\n"
            "log file: %s\n"
            "pid=%lu\n"
            "Reopen the file to refresh; Notepad won't auto-tail.\n\n",
            g_blog_path, (unsigned long)GetCurrentProcessId());
        std::fflush(g_blog_fp);
    }
}
void blog(const char* fmt, ...) {
    if (!g_debug.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(g_blog_mx);
    blog_open_once();
    if (!g_blog_fp) return;
    const unsigned long long dt = GetTickCount64() - g_blog_t0;
    std::fprintf(g_blog_fp, "[%8llu ms] ", dt);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_blog_fp, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_blog_fp);
    std::fflush(g_blog_fp);
}


// ── JNI cache ─────────────────────────────────────────────────────────────────
struct jni_cache {
    // MinecraftClient
    jclass    mc_class = nullptr;
    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_world  = nullptr;
    jfieldID  mc_player = nullptr;

    // ClientWorld — extends World; getBlockState comes off BlockView interface.
    jclass    world_class = nullptr;
    jmethodID world_getBlockState = nullptr;   // method_8320 (BlockPos)BlockState

    // ClientPlayerEntity → Entity fields
    jclass    ent_class = nullptr;
    jmethodID ent_getX = nullptr;
    jmethodID ent_getY = nullptr;
    jmethodID ent_getZ = nullptr;

    // BlockPos
    jclass    bp_class = nullptr;
    jmethodID bp_ctor  = nullptr;              // <init>(III)V

    // BlockState
    jclass    bs_class = nullptr;
    jmethodID bs_getBlock = nullptr;           // method_26204 ()Block
    jmethodID bs_isAir    = nullptr;           // method_26215 ()Z  — scanner fast-path

    // Block
    jclass    block_class = nullptr;
    jmethodID block_getDefaultMapColor = nullptr; // method_26403 ()MapColor
    jmethodID block_getName = nullptr;         // method_9518 ()Text (fallback: translation key from Item)

    // MapColor
    jclass    mc_color_class = nullptr;
    jfieldID  mc_color_field = nullptr;        // field_16011 (int color)

    // Registries.BLOCK — static field, holds a Registry<Block>.
    jclass    registries_class = nullptr;
    jfieldID  registries_block = nullptr;      // field_41175 static Registry<Block> BLOCK

    // Registry — for iteration + Block→Identifier lookup.
    jclass    registry_class = nullptr;
    jmethodID registry_getId = nullptr;        // method_10221 (Object)Identifier
    jmethodID registry_iterator = nullptr;     // iterator() from Iterable

    // Identifier
    jclass    id_class = nullptr;
    jmethodID id_toString = nullptr;           // toString()String

    // BlockUpdateS2CPacket (class_2626). Correct Yarn intermediary field
    // names verified from tools/genmaps/.cache/1.21.11/yarn_merged.tiny —
    // stable across every 1.21.x cached tiny. Earlier probes used made-up
    // numbers (field_11890 / field_29193 / field_11889 / field_29194) that
    // never existed on this class, which is why pkt.BlockUpdate=MISSING
    // persisted through 3 test runs.
    jclass    bu_pkt_class    = nullptr;
    jfieldID  bu_pkt_pos      = nullptr;   // field_12052 Lclass_2338;
    jfieldID  bu_pkt_state    = nullptr;   // field_12051 Lclass_2680;
    jmethodID bu_pkt_getPos   = nullptr;   // method_11309 — getter fallback
    jmethodID bu_pkt_getState = nullptr;   // method_11308 — getter fallback

    // ChunkDeltaUpdateS2CPacket (class_2637) — bulk block reveals in one
    // section (TNT / creeper / piston / WorldEdit paths). Without hooking
    // this, servers that leak ores via bulk deltas won't be caught.
    jclass    cd_pkt_class = nullptr;
    jfieldID  cd_pkt_sectionPos = nullptr;     // ChunkSectionPos (class_4076)
    jfieldID  cd_pkt_positions  = nullptr;     // short[]
    jfieldID  cd_pkt_states     = nullptr;     // BlockState[]

    // ChunkSectionPos (class_4076) — needed to translate the packed short[]
    // relative positions in ChunkDeltaUpdate into absolute world coords.
    jclass    csp_class = nullptr;
    jmethodID csp_getMinX = nullptr;           // method_19528
    jmethodID csp_getMinY = nullptr;           // method_19529
    jmethodID csp_getMinZ = nullptr;           // method_19530

    // ChunkDataS2CPacket (class_2672) — chunk-load coord source. We only
    // read (cx,cz) fields; the actual chunk data is applied by vanilla on
    // the client thread, so palette-walk runs in tick() after a ripen delay.
    jclass    cd_pkt2_class = nullptr;
    jfieldID  cd_pkt2_chunkX = nullptr;
    jfieldID  cd_pkt2_chunkZ = nullptr;

    // World.getChunk(int, int) → WorldChunk. Returns EmptyChunk for unloaded.
    jmethodID world_getChunk = nullptr;

    // Chunk (class_2791) parent — getSectionArray + getBottomY.
    jclass    chunk_class = nullptr;
    jmethodID chunk_getSectionArray = nullptr;
    jmethodID chunk_getBottomY = nullptr;

    // ChunkSection (class_2826) — 16^3 slab. isEmpty short-circuits per-section.
    jclass    section_class = nullptr;
    jmethodID section_isEmpty = nullptr;
    jmethodID section_getContainer = nullptr;

    // PalettedContainer (class_2841) — carries the `data` record.
    jclass    pc_class = nullptr;
    jfieldID  pc_data_field = nullptr;

    // PalettedContainer.Data record (class_2841$class_6561) — palette + storage.
    jclass    pcdata_class = nullptr;
    jfieldID  pcdata_palette = nullptr;
    jfieldID  pcdata_storage = nullptr;

    // Palette (class_2837) + PaletteStorage (class_6490).
    jclass    palette_class = nullptr;
    jmethodID palette_get = nullptr;
    jmethodID palette_getSize = nullptr;
    jclass    pstor_class = nullptr;
    jmethodID pstor_writeIndices = nullptr;

    // Reusable 4096-int scratch array (global ref, allocated once).
    jintArray pstor_scratch = nullptr;

    bool world_class_ok() const { return world_class && world_getBlockState; }
    bool render_ready() const   { return bp_class && bp_ctor && bs_getBlock; }
} C;

// Registry snapshot — populated once per world load, read every frame by the
// block-picker widget via bridge::blockPickerRows().
std::mutex                                g_reg_mx;
std::vector<vanta::vgui::bridge::BlockPickerRow> g_reg_rows;
std::unordered_map<std::string, std::uint32_t>    g_reg_color;   // id → rgb

// Cache of "we've seen this block, here's its stateId + block-id (interned)."
// Key = packed BlockPos (x:26 y:12 z:26 signed). Value = { block id index, tick_ms }.
// Index resolves back through g_reg_rows.
struct cached_block {
    std::uint32_t rgb;             // colour for render (map colour of the block)
    std::uint64_t seen_at_ms;      // insertion timestamp (diagnostics / ordering)
};

std::mutex                                       g_cache_mx;
std::unordered_map<std::int64_t, cached_block>   g_cache;
constexpr std::uint32_t kFocusRgb = 0xFFD21Fu;
std::int64_t g_focus_key = 0;
bool g_focus_active = false;
std::optional<cached_block> g_focus_previous;

// Packet-fed pending queue. Producer = netty thread (PacketReceiveEvent).
// Drained by tick() on the game thread.
struct pending_reveal {
    std::int32_t x, y, z;
    std::string  block_id;   // interned string, cheap to compare against g_reg_color
};
std::mutex                     g_pending_mx;
std::vector<pending_reveal>    g_pending;

// Chunk-load queue for the palette walker. Netty enqueues a (cx,cz), the
// game thread drains up to N per tick after a 50ms ripen delay (letting the
// vanilla ChunkData handler install the WorldChunk into ClientChunkManager).
struct pending_chunk {
    std::int32_t cx, cz;
    std::uint64_t queued_ms;
    // A refresh re-walks a chunk we have already scanned and REPLACES its
    // cached contents, so blocks placed or broken since the last walk are
    // picked up and removed respectively. Plain (non-refresh) walks only add.
    bool refresh = false;
};
std::mutex                       g_chunk_queue_mx;
std::vector<pending_chunk>       g_chunk_queue;
std::unordered_set<std::int64_t> g_chunk_recent;   // coalesce bursts

// Identity of the ClientWorld we last saw, as a global ref. Minecraft builds a
// FRESH ClientWorld on server change, dimension change and disconnect, so
// comparing object identity each tick detects all three. This exists because
// events::WorldChangeEvent is declared in Events.h but NEVER POSTED — there is
// no WorldChangeSite under src/hook/sites/, so subscribing to it was dead code
// and the cache survived across servers, drawing boxes for blocks that were
// never there.
jobject g_last_world = nullptr;

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_registry_ready{ false };

// Render-side counters — refreshed each frame, read by the tick heartbeat.
// Diagnoses "the box disappears" when the cache clearly has entries: which
// stage is dropping them (range cull? forward cull? projection?).
std::atomic<unsigned> g_dbg_render_frames{0};
std::atomic<unsigned> g_dbg_cache_seen{0};
std::atomic<unsigned> g_dbg_pass_range{0};
std::atomic<unsigned> g_dbg_pass_forward{0};
std::atomic<unsigned> g_dbg_pass_visible{0};
std::atomic<unsigned> g_dbg_edges_drawn{0};
std::atomic<unsigned> g_dbg_faces_drawn{0};
// Partial edges: segments whose far endpoint is a bisection recovery point
// rather than a real cube corner. These are the ONLY strokes that can leave
// the silhouette — every other edge joins two corners, and the convex hull
// contains every segment between them by definition. If boxes look like they
// have lines escaping while this counter reads 0, the cause is not geometry.
std::atomic<unsigned> g_dbg_partial_edges{0};

// Detection-pipeline diagnostics, reported by the heartbeat. These answer
// "why did detection stop?" — whether chunks are still arriving, whether they
// are being walked, and whether reveals are landing.
std::atomic<unsigned> g_dbg_chunkdata_pkts{0};   // ChunkData packets accepted
std::atomic<unsigned> g_dbg_chunks_walked{0};    // chunks palette-walked
std::atomic<unsigned> g_dbg_walk_hits{0};        // positions found by walks
std::atomic<unsigned> g_dbg_reveal_pkts{0};      // BlockUpdate/ChunkDelta reveals
int g_dbg_player_cx = 0, g_dbg_player_cz = 0;    // last known player chunk

// Rolling scan cursor — advances through the (x, y, z) box around the player
// each tick. A partially visited slice resumes on the next tick.
struct scan_cursor {
    int slice = 0;           // 0..39
    int last_center_x = 0;
    int last_center_y = 0;
    int last_center_z = 0;
    int range = -1;
    int resume_z = 0, resume_x = 0, resume_y = 0;
    bool resuming = false;
} g_scan;

// Pack a world position into a signed 64-bit key. 26 bits X, 12 bits Y,
// 26 bits Z — plenty for MC's ±30M x-z range and -64..319 y range.
std::int64_t pack_pos(int x, int y, int z) {
    // Cast to unsigned to avoid UB on negative shifts, then reinterpret.
    const std::uint64_t ux = static_cast<std::uint64_t>(x) & ((1ull << 26) - 1);
    const std::uint64_t uy = static_cast<std::uint64_t>(y + 64) & ((1ull << 12) - 1);
    const std::uint64_t uz = static_cast<std::uint64_t>(z) & ((1ull << 26) - 1);
    return static_cast<std::int64_t>((ux << 38) | (uy << 26) | uz);
}

void unpack_pos(std::int64_t k, int& x, int& y, int& z) {
    const std::uint64_t u = static_cast<std::uint64_t>(k);
    // Sign-extend the 26-bit x and z fields.
    auto sx = [](std::uint64_t v) {
        return static_cast<int>((v & (1ull << 25)) ? (v | ~((1ull << 26) - 1)) : v);
    };
    x = sx((u >> 38) & ((1ull << 26) - 1));
    y = static_cast<int>((u >> 26) & ((1ull << 12) - 1)) - 64;
    z = sx(u & ((1ull << 26) - 1));
}

std::string j_utf(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    return s;
}

// ── JNI resolution ──────────────────────────────────────────────────────────
void resolve_all(JNIEnv* env)
{
    // MinecraftClient — each ID individually gated (see the Entity block
    // above for the reasoning; same class of bug).
    if (!C.mc_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_310")) {
            C.mc_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.mc_class) {
        if (!C.mc_getInstance) {
            C.mc_getInstance = env->GetStaticMethodID(C.mc_class, "method_1551", "()Lnet/minecraft/class_310;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.mc_world) {
            C.mc_world = env->GetFieldID(C.mc_class, "field_1687", "Lnet/minecraft/class_638;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.mc_player) {
            C.mc_player = env->GetFieldID(C.mc_class, "field_1724", "Lnet/minecraft/class_746;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // ClientWorld / getBlockState (defined on BlockView interface, class_1922).
    if (!C.world_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_638")) {
            C.world_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.world_class && !C.world_getBlockState) {
        C.world_getBlockState = env->GetMethodID(C.world_class, "method_8320",
                                                 "(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2680;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // ClientPlayerEntity uses Entity getX/Y/Z (class_1297). Each method-ID is
    // gated on its OWN null-check — the earlier build batched all three
    // behind !ent_getX, so if getY or getZ failed to resolve on the first
    // call they'd stay null forever and live_scan would call a null
    // jmethodID (UB, JVM crash) on the second sibling.
    if (!C.ent_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1297")) {
            C.ent_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.ent_class) {
        if (!C.ent_getX) {
            C.ent_getX = env->GetMethodID(C.ent_class, "method_23317", "()D");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.ent_getY) {
            C.ent_getY = env->GetMethodID(C.ent_class, "method_23318", "()D");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.ent_getZ) {
            C.ent_getZ = env->GetMethodID(C.ent_class, "method_23321", "()D");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // BlockPos
    if (!C.bp_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2338")) {
            C.bp_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.bp_class && !C.bp_ctor) {
        C.bp_ctor = env->GetMethodID(C.bp_class, "<init>", "(III)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // BlockState
    if (!C.bs_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2680")) {
            C.bs_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.bs_class) {
        if (!C.bs_getBlock) {
            C.bs_getBlock = env->GetMethodID(C.bs_class, "method_26204", "()Lnet/minecraft/class_2248;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.bs_isAir) {
            // Fast-path for the scanner: don't do the full block-id lookup
            // on air, which is ~99% of positions in an open cave scan.
            for (const char* nm : { "method_26215", "method_26216", "isAir" }) {
                C.bs_isAir = env->GetMethodID(C.bs_class, nm, "()Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.bs_isAir) break;
            }
        }
    }

    // Block
    if (!C.block_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2248")) {
            C.block_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.block_class && !C.block_getDefaultMapColor) {
        // Names drift a bit; try the two we've seen.
        for (const char* nm : { "method_26403", "method_26404" }) {
            C.block_getDefaultMapColor = env->GetMethodID(C.block_class, nm, "()Lnet/minecraft/class_3620;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (C.block_getDefaultMapColor) break;
        }
    }

    // MapColor (class_3620) — the palette entry with an `int color` field.
    if (!C.mc_color_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_3620")) {
            C.mc_color_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.mc_color_class && !C.mc_color_field) {
        for (const char* nm : { "field_16011", "field_16008", "color" }) {
            C.mc_color_field = env->GetFieldID(C.mc_color_class, nm, "I");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (C.mc_color_field) break;
        }
    }

    // Registries + Registry.BLOCK — class_7923.field_41175 (static Registry<Block>).
    if (!C.registries_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_7923")) {
            C.registries_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.registries_class && !C.registries_block) {
        // Registries.BLOCK is actually typed as DefaultedRegistry<Block>
        // (Yarn intermediary class_7922) — NOT the base Registry class_2378.
        // esp.cpp uses the same descriptor for Registries.ITEM. Probe both
        // for defence, plus the older Registry alias if present.
        struct probe { const char* nm; const char* sig; };
        const probe probes[] = {
            { "field_41175", "Lnet/minecraft/class_7922;" },   // 1.21.x current
            { "field_41175", "Lnet/minecraft/class_2378;" },   // if Yarn ever base-types it
            { "field_11146", "Lnet/minecraft/class_2378;" },   // pre-1.19.3 alias
            { "BLOCK",       "Lnet/minecraft/class_7922;" },
            { "BLOCK",       "Lnet/minecraft/class_2378;" },
        };
        for (const auto& p : probes) {
            C.registries_block = env->GetStaticFieldID(C.registries_class, p.nm, p.sig);
            if (env->ExceptionCheck()) env->ExceptionClear();
            blog("resolve: Registries.BLOCK probe name=%s sig=%s -> %s",
                 p.nm, p.sig, C.registries_block ? "MATCH" : "no");
            if (C.registries_block) break;
        }
    }

    // Registry — walks are via Iterable.iterator(), id lookups via method_10221.
    if (!C.registry_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2378")) {
            C.registry_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.registry_class && !C.registry_getId) {
        C.registry_getId = env->GetMethodID(C.registry_class, "method_10221",
                                             "(Ljava/lang/Object;)Lnet/minecraft/class_2960;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        C.registry_iterator = env->GetMethodID(C.registry_class, "iterator",
                                                "()Ljava/util/Iterator;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Identifier
    if (!C.id_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2960")) {
            C.id_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.id_class && !C.id_toString) {
        C.id_toString = env->GetMethodID(C.id_class, "toString", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // BlockUpdateS2CPacket (class_2626) — the primary anti-xray reveal leak.
    if (!C.bu_pkt_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2626")) {
            C.bu_pkt_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.bu_pkt_class) {
        // Tier 1: correct Yarn intermediary field names first, plus the
        // deobfuscated "pos"/"state" for a remapped runtime.
        if (!C.bu_pkt_pos) {
            for (const char* nm : { "field_12052", "pos" }) {
                C.bu_pkt_pos = env->GetFieldID(C.bu_pkt_class, nm, "Lnet/minecraft/class_2338;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.bu_pkt_pos) break;
            }
        }
        if (!C.bu_pkt_state) {
            for (const char* nm : { "field_12051", "state" }) {
                C.bu_pkt_state = env->GetFieldID(C.bu_pkt_class, nm, "Lnet/minecraft/class_2680;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.bu_pkt_state) break;
            }
        }
        // Tier 2: getter methods as an independent-surface fallback. Only
        // resolved when a field probe missed, so the common path pays zero
        // extra JNI. Note: method_11310 is apply(handler), NOT getState.
        if (!C.bu_pkt_pos && !C.bu_pkt_getPos) {
            C.bu_pkt_getPos = env->GetMethodID(C.bu_pkt_class, "method_11309",
                                               "()Lnet/minecraft/class_2338;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.bu_pkt_state && !C.bu_pkt_getState) {
            C.bu_pkt_getState = env->GetMethodID(C.bu_pkt_class, "method_11308",
                                                 "()Lnet/minecraft/class_2680;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // ChunkDeltaUpdateS2CPacket (class_2637) — bulk reveal (TNT, pistons,
    // WorldEdit, cascaded flow). Anti-xray effectiveness review flagged that
    // without this, the classic Paper anti-xray engine-2 pattern (explosion
    // exposes an ore, server sends a bulk delta) is silently dropped.
    if (!C.cd_pkt_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2637")) {
            C.cd_pkt_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.cd_pkt_class) {
        // Yarn intermediary field names for class_2637, verified against the
        // Yarn cache (tools/genmaps/.cache/1.21.11/yarn_merged.tiny lines
        // 26587-26606). Stable since the 1.16.2 ChunkSection rewrite. Old
        // probes used numbers that never existed on this class (field_12126
        // was the pre-1.16.2 sectionPos; field_29179 is
        // PlayerMoveC2SPacket.onGround, unrelated to this packet).
        if (!C.cd_pkt_sectionPos) {
            for (const char* nm : { "field_26345", "sectionPos" }) {
                C.cd_pkt_sectionPos = env->GetFieldID(C.cd_pkt_class, nm, "Lnet/minecraft/class_4076;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.cd_pkt_sectionPos) break;
            }
        }
        if (!C.cd_pkt_positions) {
            for (const char* nm : { "field_26346", "positions" }) {
                C.cd_pkt_positions = env->GetFieldID(C.cd_pkt_class, nm, "[S");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.cd_pkt_positions) break;
            }
        }
        if (!C.cd_pkt_states) {
            for (const char* nm : { "field_26347", "blockStates" }) {
                C.cd_pkt_states = env->GetFieldID(C.cd_pkt_class, nm, "[Lnet/minecraft/class_2680;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.cd_pkt_states) break;
            }
        }
    }

    // ChunkSectionPos (class_4076) — decode absolute origin of a section for
    // ChunkDeltaUpdate. getMinX/Y/Z return the world-coord origin of the 16x16x16 section.
    if (!C.csp_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4076")) {
            C.csp_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.csp_class) {
        if (!C.csp_getMinX) {
            C.csp_getMinX = env->GetMethodID(C.csp_class, "method_19528", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.csp_getMinY) {
            C.csp_getMinY = env->GetMethodID(C.csp_class, "method_19529", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.csp_getMinZ) {
            C.csp_getMinZ = env->GetMethodID(C.csp_class, "method_19530", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // ── Palette-walk chain (replaces the per-position live scan) ────────────
    // ChunkDataS2CPacket
    if (!C.cd_pkt2_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2672")) {
            C.cd_pkt2_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.cd_pkt2_class) {
        if (!C.cd_pkt2_chunkX) {
            for (const char* nm : { "field_12236", "chunkX", "x" }) {
                C.cd_pkt2_chunkX = env->GetFieldID(C.cd_pkt2_class, nm, "I");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.cd_pkt2_chunkX) break;
            }
        }
        if (!C.cd_pkt2_chunkZ) {
            for (const char* nm : { "field_12235", "chunkZ", "z" }) {
                C.cd_pkt2_chunkZ = env->GetFieldID(C.cd_pkt2_class, nm, "I");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.cd_pkt2_chunkZ) break;
            }
        }
    }
    // World.getChunk lives on the world class we already resolved.
    if (C.world_class && !C.world_getChunk) {
        C.world_getChunk = env->GetMethodID(C.world_class, "method_8497",
                                            "(II)Lnet/minecraft/class_2818;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    // Chunk (class_2791)
    if (!C.chunk_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2791")) {
            C.chunk_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.chunk_class) {
        if (!C.chunk_getSectionArray) {
            C.chunk_getSectionArray = env->GetMethodID(C.chunk_class, "method_12006",
                                                       "()[Lnet/minecraft/class_2826;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.chunk_getBottomY) {
            C.chunk_getBottomY = env->GetMethodID(C.chunk_class, "method_31607", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    // ChunkSection (class_2826)
    if (!C.section_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2826")) {
            C.section_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.section_class) {
        if (!C.section_isEmpty) {
            for (const char* nm : { "method_38292", "method_38294", "isEmpty" }) {
                C.section_isEmpty = env->GetMethodID(C.section_class, nm, "()Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.section_isEmpty) break;
            }
        }
        if (!C.section_getContainer) {
            C.section_getContainer = env->GetMethodID(C.section_class, "method_12265",
                                                      "()Lnet/minecraft/class_2841;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    // PalettedContainer (class_2841)
    if (!C.pc_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2841")) {
            C.pc_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.pc_class && !C.pc_data_field) {
        for (const char* nm : { "field_34560", "data" }) {
            C.pc_data_field = env->GetFieldID(C.pc_class, nm,
                                              "Lnet/minecraft/class_2841$class_6561;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (C.pc_data_field) break;
        }
    }
    // PalettedContainer.Data record (class_2841$class_6561)
    if (!C.pcdata_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env,
                       "net/minecraft/class_2841$class_6561")) {
            C.pcdata_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.pcdata_class) {
        if (!C.pcdata_palette) {
            for (const char* nm : { "comp_119", "palette" }) {
                C.pcdata_palette = env->GetFieldID(C.pcdata_class, nm,
                                                   "Lnet/minecraft/class_2837;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.pcdata_palette) break;
            }
        }
        if (!C.pcdata_storage) {
            for (const char* nm : { "comp_118", "storage" }) {
                C.pcdata_storage = env->GetFieldID(C.pcdata_class, nm,
                                                   "Lnet/minecraft/class_6490;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.pcdata_storage) break;
            }
        }
    }
    // Palette
    if (!C.palette_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2837")) {
            C.palette_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.palette_class) {
        if (!C.palette_get) {
            C.palette_get = env->GetMethodID(C.palette_class, "method_12288",
                                             "(I)Ljava/lang/Object;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!C.palette_getSize) {
            C.palette_getSize = env->GetMethodID(C.palette_class, "method_12197", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    // PaletteStorage
    if (!C.pstor_class) {
        if (jclass k = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_6490")) {
            C.pstor_class = static_cast<jclass>(env->NewGlobalRef(k));
            env->DeleteLocalRef(k);
        }
    }
    if (C.pstor_class && !C.pstor_writeIndices) {
        for (const char* nm : { "method_39892", "writePaletteIndices" }) {
            C.pstor_writeIndices = env->GetMethodID(C.pstor_class, nm, "([I)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (C.pstor_writeIndices) break;
        }
    }

    // Reusable 4096-int scratch array (one-time alloc).
    if (!C.pstor_scratch) {
        jintArray a = env->NewIntArray(4096);
        if (a) { C.pstor_scratch = (jintArray)env->NewGlobalRef(a); env->DeleteLocalRef(a); }
    }
}

// Read block identifier ("minecraft:crafting_table") from a jobject BlockState.
std::string block_state_id(JNIEnv* env, jobject state)
{
    // Guard EVERY id we're about to touch — passing a null jclass or
    // jfieldID to GetStaticObjectField is UB per the JNI spec (adversarial
    // review flagged this; enumerate_registry already guards the same
    // combination). Missing any of these leaves the caller with an empty
    // string, which the caller treats as "not in target set".
    if (!state || !C.bs_getBlock || !C.registry_getId || !C.id_toString ||
        !C.registries_class || !C.registries_block) return {};

    jobject block = env->CallObjectMethod(state, C.bs_getBlock);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (!block) return {};

    jobject registry = env->GetStaticObjectField(C.registries_class, C.registries_block);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(block); return {}; }

    jobject id = env->CallObjectMethod(registry, C.registry_getId, block);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    std::string s;
    if (id) {
        jstring js = static_cast<jstring>(env->CallObjectMethod(id, C.id_toString));
        if (env->ExceptionCheck()) env->ExceptionClear();
        s = j_utf(env, js);
        if (js) env->DeleteLocalRef(js);
        env->DeleteLocalRef(id);
    }
    if (registry) env->DeleteLocalRef(registry);
    env->DeleteLocalRef(block);
    return s;
}

// Read the map-colour rgb (0x00RRGGBB) for a given Block jobject. Zero if
// anything failed to resolve — the render then falls back to the user's
// custom colour or a default violet.
// Stable per-identifier fallback colour. Used when the MapColor JNI chain
// can't be resolved on this build — without it every block falls back to the
// single custom colour and the whole selection renders identically, which is
// exactly the "todos los bloques del mismo color" report.
//
// Hue comes from a hash of the identifier so a given block always gets the
// same colour; saturation and value are pinned high so the boxes stay legible
// against the world.
std::uint32_t id_hash_color(const std::string& id)
{
    std::uint32_t h = 2166136261u;
    for (char c : id) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }

    const float hue = static_cast<float>(h % 360u) / 360.f;
    const float s = 0.72f, v = 1.0f;
    const float hp = hue * 6.f;
    const int   i  = static_cast<int>(hp) % 6;
    const float f  = hp - static_cast<float>(static_cast<int>(hp));
    const float p = v * (1.f - s);
    const float q = v * (1.f - s * f);
    const float t = v * (1.f - s * (1.f - f));
    float r = 0.f, g = 0.f, b = 0.f;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    const std::uint32_t R = static_cast<std::uint32_t>(r * 255.f) & 0xFFu;
    const std::uint32_t G = static_cast<std::uint32_t>(g * 255.f) & 0xFFu;
    const std::uint32_t B = static_cast<std::uint32_t>(b * 255.f) & 0xFFu;
    // Never return 0 — the render treats 0 as "no colour known".
    const std::uint32_t out = (R << 16) | (G << 8) | B;
    return out ? out : 0x404040u;
}

// Curated colours for the blocks people actually search for.
//
// MapColor — which is what Block.getDefaultMapColor() returns — is the palette
// Minecraft uses to draw MAPS, and it only has ~62 entries. Every ore in the
// game maps to STONE_GRAY there, because an ore block is mostly stone and that
// is how it should look on a map. So MapColor is technically correct and
// completely useless for telling diamond from coal: the log confirmed 1018
// blocks resolved a real MapColor, yet every ore came out the same grey while
// crafting_table (MapColor = wood brown) looked right.
//
// These are the perceptual colours of the mineral itself, which is what
// "el color predominante" means for an ore. Matched on a substring of the
// path so the deepslate_/nether_/raw_ variants are all covered by one rule.
// Order matters: more specific keys must come before their substrings.
std::uint32_t curated_block_color(const std::string& id)
{
    const auto colon = id.find(':');
    const std::string path = (colon == std::string::npos) ? id : id.substr(colon + 1);

    struct rule { const char* key; std::uint32_t rgb; };
    static const rule kRules[] = {
        // Ores + their raw/storage blocks.
        { "ancient_debris",  0x6E4B3Au },
        { "debris",          0x6E4B3Au },
        { "diamond",         0x4AEDD9u },
        { "emerald",         0x17DD62u },
        { "lapis",           0x1A4BA8u },
        { "redstone",        0xFF3030u },
        { "amethyst",        0xA678E8u },
        { "quartz",          0xEAE5DEu },
        { "gold",            0xFCEE4Bu },
        { "copper",          0xE77C56u },
        { "iron",            0xD8AF93u },
        { "coal",            0x343434u },
        // Containers / points of interest.
        { "spawner",         0x1B4C4Cu },
        { "trapped_chest",   0xA0522Du },
        { "ender_chest",     0x2C4A47u },
        { "chest",           0xC08A3Eu },
        { "shulker_box",     0x9A5EA1u },
        { "barrel",          0xB07B3Eu },
        { "furnace",         0x8A8A8Au },
        { "beacon",          0x7DE3D8u },
        { "portal",          0x8B4AC4u },
        { "obsidian",        0x1A1024u },
        { "budding",         0xB98CE8u },
        { "sculk",           0x0F4A50u },
        { "bee_nest",        0xD9A441u },
        { "beehive",         0xD9A441u },
        { "tnt",             0xD63A2Fu },
    };

    for (const auto& r : kRules) {
        if (path.find(r.key) != std::string::npos) return r.rgb;
    }
    return 0;   // no curated entry — caller falls back to MapColor
}

std::uint32_t block_map_color(JNIEnv* env, jobject block)
{
    if (!block || !C.block_getDefaultMapColor || !C.mc_color_field) return 0;
    jobject mc_color = env->CallObjectMethod(block, C.block_getDefaultMapColor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (!mc_color) return 0;
    const jint rgb = env->GetIntField(mc_color, C.mc_color_field);
    env->DeleteLocalRef(mc_color);
    return static_cast<std::uint32_t>(rgb) & 0x00FFFFFFu;
}

// ── Registry snapshot ──────────────────────────────────────────────────────
void enumerate_registry(JNIEnv* env)
{
    if (g_registry_ready.load(std::memory_order_acquire)) return;
    if (!C.registries_class || !C.registries_block || !C.registry_iterator) {
        static bool once = false;
        if (!once) {
            once = true;
            blog("enumerate: skipping — registries_class=%s registries_block=%s iter=%s",
                 C.registries_class ? "ok" : "MISSING",
                 C.registries_block ? "ok" : "MISSING",
                 C.registry_iterator ? "ok" : "MISSING");
        }
        return;
    }

    jobject registry = env->GetStaticObjectField(C.registries_class, C.registries_block);
    if (env->ExceptionCheck() || !registry) {
        env->ExceptionClear();
        blog("enumerate: GetStaticObjectField(Registries.BLOCK) returned NULL");
        return;
    }

    jobject iter = env->CallObjectMethod(registry, C.registry_iterator);
    if (env->ExceptionCheck() || !iter) {
        env->ExceptionClear();
        env->DeleteLocalRef(registry);
        return;
    }

    jclass iter_cls = env->GetObjectClass(iter);
    jmethodID has_next = env->GetMethodID(iter_cls, "hasNext", "()Z");
    jmethodID next     = env->GetMethodID(iter_cls, "next", "()Ljava/lang/Object;");

    std::vector<vanta::vgui::bridge::BlockPickerRow> rows;
    std::unordered_map<std::string, std::uint32_t>   colors;
    rows.reserve(1024);
    // Track which colour path each block took so the log can say whether the
    // MapColor JNI chain is actually working on this build.
    std::size_t real_colors = 0, fallback_colors = 0, curated_colors = 0;

    int guard = 0;
    while (has_next && next && env->CallBooleanMethod(iter, has_next) == JNI_TRUE) {
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject block = env->CallObjectMethod(iter, next);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!block) continue;

        jobject id = env->CallObjectMethod(registry, C.registry_getId, block);
        if (env->ExceptionCheck()) env->ExceptionClear();
        std::string ids;
        if (id) {
            jstring js = static_cast<jstring>(env->CallObjectMethod(id, C.id_toString));
            if (env->ExceptionCheck()) env->ExceptionClear();
            ids = j_utf(env, js);
            if (js) env->DeleteLocalRef(js);
            env->DeleteLocalRef(id);
        }
        std::uint32_t rgb = block_map_color(env, block);
        env->DeleteLocalRef(block);

        if (!ids.empty()) {
            // Colour priority:
            //   1. Curated — the mineral's real colour. MapColor collapses
            //      EVERY ore to stone grey, so this is the only way diamond
            //      and coal look different.
            //   2. MapColor — the block's own predominant colour. Correct and
            //      distinctive for everything that isn't an ore.
            //   3. Identifier hash — guarantees distinct colours if the
            //      MapColor JNI chain is unavailable on this build.
            if (const std::uint32_t cur = curated_block_color(ids)) {
                rgb = cur; ++curated_colors;
            } else if (rgb != 0) {
                ++real_colors;
            } else {
                rgb = id_hash_color(ids); ++fallback_colors;
            }
            // Human label = the part after ':' with underscores → spaces + title case.
            const auto colon = ids.find(':');
            std::string label = colon == std::string::npos ? ids : ids.substr(colon + 1);
            for (auto& c : label) if (c == '_') c = ' ';
            if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            for (std::size_t i = 1; i < label.size(); ++i) {
                if (label[i - 1] == ' ')
                    label[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[i])));
            }
            colors[ids] = rgb;
            rows.push_back({ ids, std::move(label), rgb });
        }

        if (++guard > 8192) break;   // safety net; MC has ~1200 blocks
    }
    env->DeleteLocalRef(iter);
    env->DeleteLocalRef(iter_cls);
    env->DeleteLocalRef(registry);

    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    {
        std::lock_guard lk(g_reg_mx);
        g_reg_rows  = std::move(rows);
        g_reg_color = std::move(colors);
    }
    g_registry_ready.store(true, std::memory_order_release);
    blog("enumerate: colours — %zu curated, %zu MapColor, %zu identifier-hash "
         "(mapcolor_method=%s mapcolor_field=%s)",
         curated_colors, real_colors, fallback_colors,
         C.block_getDefaultMapColor ? "ok" : "MISSING",
         C.mc_color_field ? "ok" : "MISSING");
    VLOG_INFO("blockesp", "registry snapshot: {} blocks", (int)g_reg_rows.size());
    blog("enumerate: registry snapshot ready — %d blocks", (int)g_reg_rows.size());
}

// ── Scanner ────────────────────────────────────────────────────────────────
void live_scan(JNIEnv* env)
{
    // Every sibling ID is required for the loop body; per-ID guards keep us
    // safe against a partial resolve_all outcome.
    if (!C.mc_class || !C.mc_getInstance || !C.mc_world || !C.mc_player) return;
    if (!C.world_getBlockState || !C.bp_class || !C.bp_ctor) return;
    if (!C.ent_getX || !C.ent_getY || !C.ent_getZ) return;

    jobject client = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck() || !client) { env->ExceptionClear(); return; }

    jobject world  = env->GetObjectField(client, C.mc_world);
    jobject player = env->GetObjectField(client, C.mc_player);
    if (!world || !player) {
        if (world)  env->DeleteLocalRef(world);
        if (player) env->DeleteLocalRef(player);
        env->DeleteLocalRef(client);
        return;
    }

    // Snapshot the target set into an unordered_set for O(1) match.
    std::unordered_set<std::string> targets;
    { std::scoped_lock lk(g_settings_mx); for (const auto& s : g_settings.target_ids) targets.insert(s); }
    if (targets.empty()) {
        env->DeleteLocalRef(player); env->DeleteLocalRef(world); env->DeleteLocalRef(client); return;
    }

    // Latch the player centre at the start of each 40-tick sweep and USE it
    // for every following slice. The earlier build wrote last_center_* but
    // read the live px/py/pz on every slice, so a player who rose during the
    // sweep skipped entire Y layers. Recentre once per full sweep so the
    // vertical slab is stable across the sweep.
    constexpr int kSlices = 40;
    const int R = std::clamp(g_settings.range, 4, 128);
    if (g_scan.range != R) { g_scan = {}; g_scan.range = R; }
    if (g_scan.slice == 0 && !g_scan.resuming) {
        g_scan.last_center_x = static_cast<int>(std::floor(env->CallDoubleMethod(player, C.ent_getX)));
        g_scan.last_center_y = static_cast<int>(std::floor(env->CallDoubleMethod(player, C.ent_getY)));
        g_scan.last_center_z = static_cast<int>(std::floor(env->CallDoubleMethod(player, C.ent_getZ)));
    }
    env->DeleteLocalRef(player);

    const int px = g_scan.last_center_x;
    const int py = g_scan.last_center_y;
    const int pz = g_scan.last_center_z;

    const int minY = std::max(-64, py - R / 2);
    const int maxY = std::min(319, py + R / 2);
    const int span = (maxY - minY) + 1;   // +1 so slice_hi at slice=39 covers maxY
    if (span <= 0) { env->DeleteLocalRef(world); env->DeleteLocalRef(client); return; }

    const int slice_lo = minY + (span * g_scan.slice)       / kSlices;
    const int slice_hi = minY + (span * (g_scan.slice + 1)) / kSlices;
    if (slice_hi <= slice_lo) {
        g_scan.slice = (g_scan.slice + 1) % kSlices;
        g_scan.resuming = false;
        env->DeleteLocalRef(world); env->DeleteLocalRef(client);
        return;
    }

    // Fast air-skip: bs_isAir is a single-int-comparison inside MC. Skipping
    // it wastes a getBlockId lookup per air cell — ~99% of an open scan.
    // Falls back to always-treat-as-solid if isAir failed to resolve.
    const bool have_air_fastpath = (C.bs_isAir != nullptr);

    // BlockState identity fast-path: many neighbour blocks share the same
    // interned BlockState (stone-run in cave scans, air past isAir, sand
    // etc.). Caching the previous state's IsSameObject match saves the
    // block→id→map-color chain (~5 JNI calls) whenever we get a run.
    // A local-ref-scoped copy of the "last seen" state is kept in a global
    // ref so it survives across the CallObjectMethod that returns a fresh
    // local ref for the current cell. Reset per-slice so stale refs don't
    // dangle across chunk unloads.
    jobject last_state_global = nullptr;
    bool    last_was_target   = false;
    std::uint32_t last_rgb    = 0;
    std::string   last_id;

    std::vector<std::pair<std::int64_t, std::uint32_t>> fresh;
    fresh.reserve(128);

    // Cooperative budget on a single scan slice — even at R=128 with a full 3-block
    // Y slab, we cap at 3ms of JNI work so the render thread's headroom is
    // preserved. Whatever the loop hasn't covered rolls into the next tick
    // via the slice cursor. Rough goal: 100+ FPS at max range.
    vanta::util::TimeBudget budget(std::chrono::microseconds(3000));
    bool budget_hit = false;

    // Reusable BlockPos to avoid NewObject per cell — BlockPos is immutable
    // in Yarn, but there is a Mutable subclass. Falling back to per-cell
    // NewObject keeps the code simple; the throttle already caps rate.
    const bool resuming = g_scan.resuming;
    const int first_z = resuming ? g_scan.resume_z : -R;
    auto pause = [&](int z, int x, int y) {
        g_scan.resume_z = z; g_scan.resume_x = x; g_scan.resume_y = y;
        g_scan.resuming = true; budget_hit = true;
    };
    for (int dz = first_z; dz <= R && !budget_hit; ++dz) {
        const int first_x = resuming && dz == first_z ? g_scan.resume_x : -R;
        for (int dx = first_x; dx <= R && !budget_hit; ++dx) {
            const int first_y = resuming && dz == first_z && dx == first_x ? g_scan.resume_y : slice_lo;
            if (budget.expired()) { pause(dz, dx, first_y); break; }
            if (dx * dx + dz * dz > R * R) continue;
            for (int y = first_y; y < slice_hi; ++y) {
                if (budget.expired()) { pause(dz, dx, y); break; }
                const int wx = px + dx, wz = pz + dz;
                jobject pos = env->NewObject(C.bp_class, C.bp_ctor,
                                             (jint)wx, (jint)y, (jint)wz);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                if (!pos) continue;

                jobject state = env->CallObjectMethod(world, C.world_getBlockState, pos);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(pos);
                if (!state) continue;

                // Fast identity match: if the returned state is the SAME
                // interned object as the previous one, reuse the cached
                // target decision. Avoids ~5 JNI calls per repeated cell.
                if (last_state_global && env->IsSameObject(state, last_state_global) == JNI_TRUE) {
                    env->DeleteLocalRef(state);
                    if (last_was_target)
                        fresh.emplace_back(pack_pos(wx, y, wz), last_rgb);
                    continue;
                }

                if (have_air_fastpath) {
                    const jboolean air = env->CallBooleanMethod(state, C.bs_isAir);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (air == JNI_TRUE) {
                        // Rotate the identity cache to "air" so the next
                        // air cell also short-circuits.
                        if (last_state_global) env->DeleteGlobalRef(last_state_global);
                        last_state_global = env->NewGlobalRef(state);
                        last_was_target = false; last_rgb = 0; last_id.clear();
                        env->DeleteLocalRef(state);
                        continue;
                    }
                }

                const std::string id = block_state_id(env, state);
                if (last_state_global) env->DeleteGlobalRef(last_state_global);
                last_state_global = env->NewGlobalRef(state);
                env->DeleteLocalRef(state);
                last_id = id;

                if (id.empty() || !targets.count(id)) {
                    last_was_target = false; last_rgb = 0;
                    continue;
                }

                std::uint32_t rgb = 0;
                {
                    std::lock_guard lk(g_reg_mx);
                    auto it = g_reg_color.find(id);
                    if (it != g_reg_color.end()) rgb = it->second;
                }
                last_was_target = true; last_rgb = rgb;
                fresh.emplace_back(pack_pos(wx, y, wz), rgb);
            }
        }
    }
    if (!budget_hit) {
        g_scan.resuming = false;
        g_scan.slice = (g_scan.slice + 1) % kSlices;
    }
    if (last_state_global) env->DeleteGlobalRef(last_state_global);

    env->DeleteLocalRef(world);
    env->DeleteLocalRef(client);

    if (!fresh.empty()) {
        std::lock_guard lk(g_cache_mx);
        const std::uint64_t now = static_cast<std::uint64_t>(GetTickCount64());
        for (auto& [k, rgb] : fresh) g_cache[k] = { rgb, now };

        // Cap cache size — at R=128 with a densely-populated target (e.g.
        // "stone" in a mountain biome), the cache could balloon into the
        // hundreds of thousands and both memory and per-frame render cost
        // would explode. When over the cap, drop the OLDEST entries until
        // we're back under. Practical for typical target sets (ores, chests,
        // spawners) that never come close to the cap in the first place.
        constexpr std::size_t kCacheCap = 8192;
        if (g_cache.size() > kCacheCap) {
            std::vector<std::pair<std::uint64_t, std::int64_t>> by_age;
            by_age.reserve(g_cache.size());
            for (const auto& [k, v] : g_cache) by_age.emplace_back(v.seen_at_ms, k);
            std::partial_sort(by_age.begin(),
                              by_age.begin() + (g_cache.size() - kCacheCap),
                              by_age.end());
            for (std::size_t i = 0; i < g_cache.size() - kCacheCap; ++i) {
                g_cache.erase(by_age[i].second);
            }
        }
    }

    // Cache entries are never time-decayed. Staleness is handled structurally:
    // the nearby refresh replaces what it re-walks, movement prunes beyond 2x
    // range, and a world change drops everything.
}

// Drain the reveal-packet queue into the same cache.
void drain_reveals()
{
    std::vector<pending_reveal> local;
    {
        std::lock_guard lk(g_pending_mx);
        local.swap(g_pending);
    }
    if (local.empty()) return;

    std::unordered_set<std::string> targets;
    { std::scoped_lock lk(g_settings_mx); for (const auto& s : g_settings.target_ids) targets.insert(s); }
    // Note: don't return-on-empty-targets here — even with no targets, we
    // still want to drain the queue so it doesn't grow unbounded.

    std::lock_guard lk(g_cache_mx);
    const std::uint64_t now = static_cast<std::uint64_t>(GetTickCount64());
    for (auto& r : local) {
        const std::int64_t key = pack_pos(r.x, r.y, r.z);
        if (targets.empty() || !targets.count(r.block_id)) {
            // Non-target reveal at a cached position → EVICT. This fixes
            // Paper anti-xray fake-ore poisoning: the initial chunk-load
            // shows a fake ore, then the server sends BlockUpdate(pos, stone)
            // to un-fake once the player is close. Without this eviction the
            // phantom ore lingers in the cache forever.
            auto it = g_cache.find(key);
            if (it != g_cache.end()) g_cache.erase(it);
            continue;
        }
        std::uint32_t rgb = 0;
        {
            std::lock_guard lk2(g_reg_mx);
            auto rit = g_reg_color.find(r.block_id);
            if (rit != g_reg_color.end()) rgb = rit->second;
        }
        g_cache[key] = { rgb, now };
    }
}

// ── Packet listener ────────────────────────────────────────────────────────
//
// Two packet shapes carry block reveals: BlockUpdateS2CPacket (single block,
// e.g. a neighbour break exposing an ore) and ChunkDeltaUpdateS2CPacket
// (bulk — TNT, pistons, WorldEdit, cascaded flow). Hooking only the former
// misses everything anti-xray engine-2 leaks through bulk deltas.

void handle_block_update(JNIEnv* env, jobject packet)
{
    // Fields preferred; fall through to the getter methods if fields missed
    // (extreme Yarn drift). At least one of each pair must be resolved.
    const bool have_pos_field    = C.bu_pkt_pos != nullptr;
    const bool have_state_field  = C.bu_pkt_state != nullptr;
    const bool have_pos_method   = C.bu_pkt_getPos != nullptr;
    const bool have_state_method = C.bu_pkt_getState != nullptr;
    if (!(have_pos_field || have_pos_method))     return;
    if (!(have_state_field || have_state_method)) return;

    jobject pos = have_pos_field
        ? env->GetObjectField(packet, C.bu_pkt_pos)
        : env->CallObjectMethod(packet, C.bu_pkt_getPos);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    jobject st = have_state_field
        ? env->GetObjectField(packet, C.bu_pkt_state)
        : env->CallObjectMethod(packet, C.bu_pkt_getState);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (pos) env->DeleteLocalRef(pos); return; }

    if (!pos || !st) {
        if (pos) env->DeleteLocalRef(pos);
        if (st)  env->DeleteLocalRef(st);
        return;
    }

    // BlockPos getters are cheap ints. Look them up off the actual class of
    // the instance (safer than assuming the cached C.bp_class matches).
    jclass bpcls = env->GetObjectClass(pos);
    jmethodID gx = env->GetMethodID(bpcls, "method_10263", "()I");
    jmethodID gy = env->GetMethodID(bpcls, "method_10264", "()I");
    jmethodID gz = env->GetMethodID(bpcls, "method_10260", "()I");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!gx || !gy || !gz) {
        env->DeleteLocalRef(bpcls); env->DeleteLocalRef(pos); env->DeleteLocalRef(st); return;
    }

    const int x = env->CallIntMethod(pos, gx);
    const int y = env->CallIntMethod(pos, gy);
    const int z = env->CallIntMethod(pos, gz);
    env->DeleteLocalRef(bpcls);
    env->DeleteLocalRef(pos);

    std::string id = block_state_id(env, st);
    env->DeleteLocalRef(st);
    if (id.empty()) return;

    {
        std::lock_guard lk(g_pending_mx);
        g_pending.push_back({ x, y, z, std::move(id) });
    }
    g_dbg_reveal_pkts.fetch_add(1, std::memory_order_relaxed);
}

void handle_chunk_delta(JNIEnv* env, jobject packet)
{
    if (!C.cd_pkt_sectionPos || !C.cd_pkt_positions || !C.cd_pkt_states) return;
    if (!C.csp_getMinX || !C.csp_getMinY || !C.csp_getMinZ) return;

    jobject secPos = env->GetObjectField(packet, C.cd_pkt_sectionPos);
    jshortArray positions = static_cast<jshortArray>(env->GetObjectField(packet, C.cd_pkt_positions));
    jobjectArray states   = static_cast<jobjectArray>(env->GetObjectField(packet, C.cd_pkt_states));
    if (!secPos || !positions || !states) {
        if (secPos)    env->DeleteLocalRef(secPos);
        if (positions) env->DeleteLocalRef(positions);
        if (states)    env->DeleteLocalRef(states);
        return;
    }

    const int baseX = env->CallIntMethod(secPos, C.csp_getMinX);
    const int baseY = env->CallIntMethod(secPos, C.csp_getMinY);
    const int baseZ = env->CallIntMethod(secPos, C.csp_getMinZ);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(secPos);

    const jsize n = env->GetArrayLength(positions);
    // Cap absurd payloads so a hostile server can't wedge the netty thread.
    const jsize cap = std::min<jsize>(n, 8192);

    jshort* raw = env->GetShortArrayElements(positions, nullptr);
    if (!raw) {
        env->DeleteLocalRef(positions); env->DeleteLocalRef(states); return;
    }

    std::vector<pending_reveal> local;
    local.reserve(cap);

    for (jsize i = 0; i < cap; ++i) {
        const unsigned short packed = static_cast<unsigned short>(raw[i]);
        // Yarn PackedIntegerArray-style encoding used by ChunkDelta:
        //   x = (packed >> 8) & 0xF
        //   z = (packed >> 4) & 0xF
        //   y =  packed       & 0xF
        // (see net.minecraft.util.math.ChunkSectionPos.unpackLocal*)
        const int lx = (packed >> 8) & 0xF;
        const int lz = (packed >> 4) & 0xF;
        const int ly =  packed       & 0xF;

        jobject st = env->GetObjectArrayElement(states, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!st) continue;

        std::string id = block_state_id(env, st);
        env->DeleteLocalRef(st);
        if (id.empty()) continue;

        local.push_back({ baseX + lx, baseY + ly, baseZ + lz, std::move(id) });
    }
    env->ReleaseShortArrayElements(positions, raw, JNI_ABORT);
    env->DeleteLocalRef(positions);
    env->DeleteLocalRef(states);

    if (!local.empty()) {
        std::lock_guard lk(g_pending_mx);
        for (auto& r : local) g_pending.push_back(std::move(r));
    }
}

// ── Palette walk of a single loaded chunk ──────────────────────────────────
// Reads the PalettedContainer.Data record for each ChunkSection, walks the
// 4096-cell index array, and pushes every position that matches the target
// set into `out`. Returns true on best-effort completion — false only when
// a JNI ID chain isn't resolved (so the caller can retry later). Empty chunks
// (unloaded coords) walk cleanly with zero output.
bool palette_walk_chunk(JNIEnv* env, jobject world, int cx, int cz,
                        const std::unordered_set<std::string>& targets,
                        std::vector<std::pair<std::int64_t, std::uint32_t>>& out)
{
    if (!C.world_getChunk || !C.chunk_getSectionArray || !C.chunk_getBottomY) return false;
    if (!C.section_getContainer || !C.pc_data_field) return false;
    if (!C.pcdata_palette || !C.pcdata_storage) return false;
    if (!C.palette_get || !C.palette_getSize) return false;
    if (!C.pstor_writeIndices || !C.pstor_scratch) return false;

    jobject chunk = env->CallObjectMethod(world, C.world_getChunk, (jint)cx, (jint)cz);
    if (env->ExceptionCheck() || !chunk) { env->ExceptionClear(); if (chunk) env->DeleteLocalRef(chunk); return true; }

    const jint bottomY = env->CallIntMethod(chunk, C.chunk_getBottomY);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(chunk); return true; }

    jobjectArray secs = (jobjectArray)env->CallObjectMethod(chunk, C.chunk_getSectionArray);
    env->DeleteLocalRef(chunk);
    if (env->ExceptionCheck() || !secs) { env->ExceptionClear(); if (secs) env->DeleteLocalRef(secs); return true; }

    std::vector<jint> indices(4096);

    const jsize nSec = env->GetArrayLength(secs);
    for (jsize si = 0; si < nSec; ++si) {
        jobject section = env->GetObjectArrayElement(secs, si);
        if (env->ExceptionCheck() || !section) { env->ExceptionClear(); continue; }

        if (C.section_isEmpty &&
            env->CallBooleanMethod(section, C.section_isEmpty) == JNI_TRUE) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(section);
            continue;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();

        jobject pc = env->CallObjectMethod(section, C.section_getContainer);
        env->DeleteLocalRef(section);
        if (env->ExceptionCheck() || !pc) { env->ExceptionClear(); if (pc) env->DeleteLocalRef(pc); continue; }

        jobject data = env->GetObjectField(pc, C.pc_data_field);
        env->DeleteLocalRef(pc);
        if (!data) continue;

        jobject palette = env->GetObjectField(data, C.pcdata_palette);
        jobject storage = env->GetObjectField(data, C.pcdata_storage);
        env->DeleteLocalRef(data);
        if (!palette || !storage) {
            if (palette) env->DeleteLocalRef(palette);
            if (storage) env->DeleteLocalRef(storage);
            continue;
        }

        const jint pn = env->CallIntMethod(palette, C.palette_getSize);
        if (env->ExceptionCheck() || pn <= 0 || pn > 4096) {
            env->ExceptionClear();
            env->DeleteLocalRef(palette); env->DeleteLocalRef(storage);
            continue;
        }
        // Build per-palette-index "is target + rgb" table. Top bit marks hit.
        std::vector<std::uint32_t> pal_rgb(pn, 0);
        bool any_hit = false;
        for (jint pi = 0; pi < pn; ++pi) {
            jobject bstate = env->CallObjectMethod(palette, C.palette_get, pi);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!bstate) continue;
            const std::string id = block_state_id(env, bstate);
            env->DeleteLocalRef(bstate);
            if (id.empty() || !targets.count(id)) continue;
            std::uint32_t rgb = 0;
            {
                std::lock_guard lk(g_reg_mx);
                auto it = g_reg_color.find(id);
                if (it != g_reg_color.end()) rgb = it->second;
            }
            pal_rgb[pi] = rgb | 0xFF000000u;
            any_hit = true;
        }
        env->DeleteLocalRef(palette);

        if (!any_hit) { env->DeleteLocalRef(storage); continue; }

        // Bulk-fill the 4096 palette indices in ONE JNI call. This is the
        // whole point of palette walk — no per-cell getBlockState.
        env->CallVoidMethod(storage, C.pstor_writeIndices, C.pstor_scratch);
        env->DeleteLocalRef(storage);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        env->GetIntArrayRegion(C.pstor_scratch, 0, 4096, indices.data());
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

        // Native walk. PaletteStorage cell layout: y*256 + z*16 + x.
        const int sectionBaseY = bottomY + (int)si * 16;
        for (int cell = 0; cell < 4096; ++cell) {
            const jint idx = indices[cell];
            if (idx < 0 || idx >= pn) continue;
            const std::uint32_t flag = pal_rgb[idx];
            if (!(flag & 0xFF000000u)) continue;
            const int lx = cell & 15;
            const int lz = (cell >> 4) & 15;
            const int ly = (cell >> 8) & 15;
            out.emplace_back(
                pack_pos((cx << 4) + lx, sectionBaseY + ly, (cz << 4) + lz),
                flag & 0x00FFFFFFu);
        }
    }
    env->DeleteLocalRef(secs);
    return true;
}

bool enqueue_loaded_chunks_impl(JNIEnv* env);            // defined below
bool player_chunk(JNIEnv* env, int& out_cx, int& out_cz); // defined below
void prune_far_cache(int px, int pz);                     // defined below

// Deferred re-seed state. A world change cannot seed immediately — the player
// entity has not spawned yet and reads as being at the origin. Instead we arm
// a retry and let tick() attempt it until it succeeds.
bool          g_reseed_pending = false;
std::uint64_t g_reseed_at_ms   = 0;
std::uint64_t g_last_seed_ms   = 0;
std::uint64_t g_last_refresh_ms = 0;

// Chunk coords the last successful seed was centred on, and its radius. The
// seed only covers a disc around that point, so once the player walks out of
// it nothing re-triggers a scan and the cache is left full of far-away blocks
// that all fail the range cull — detections stop even though the module looks
// healthy. Tracking the origin lets us re-seed on movement.
bool g_seed_valid = false;
int  g_seed_cx = 0, g_seed_cz = 0, g_seed_rc = 0;

// Hash of the target-id set as of the last scan. Picking a block in the
// ClickGui has to re-walk everything immediately: the chunks around the player
// are already in g_chunk_recent from the previous scan, so without this the
// new selection only appears when the player walks into fresh chunks or the
// 5 s empty-cache heal fires — which is the "tarda en renderizarse" delay.
std::size_t g_last_targets_hash = 0;

std::size_t hash_targets(const std::vector<std::string>& ids)
{
    std::size_t h = 1469598103934665603ull;
    for (const auto& s : ids) {
        for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
        h ^= 0x9E3779B97F4A7C15ull;
    }
    return h ^ (ids.size() * 1000003ull);
}

// ── World-change detection ─────────────────────────────────────────────────
//
// Purges every cached position when the ClientWorld object identity changes.
// Covers: joining a different server, changing dimension (Nether/End portal),
// disconnecting to the menu, and loading a singleplayer world — MC allocates a
// new ClientWorld in all of those cases.
//
// Without this the cache is keyed on bare (x,y,z) with no world discriminator,
// so blocks found on one server keep drawing at the same coordinates on the
// next one. Runs regardless of g_enabled so a disabled module doesn't hold a
// stale cache that flashes on re-enable.
void detect_world_change(JNIEnv* env)
{
    if (!C.mc_class || !C.mc_getInstance || !C.mc_world) return;

    jobject client = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!client) return;

    jobject world = env->GetObjectField(client, C.mc_world);
    env->DeleteLocalRef(client);
    if (env->ExceptionCheck()) { env->ExceptionClear(); if (world) env->DeleteLocalRef(world); return; }

    // Same world? Nothing to do. IsSameObject also correctly reports true for
    // two nulls (both "no world loaded").
    const bool same = env->IsSameObject(world, g_last_world) == JNI_TRUE;
    if (same) { if (world) env->DeleteLocalRef(world); return; }

    // Changed. Re-anchor first so a failure below can't leave us re-purging
    // every tick.
    if (g_last_world) { env->DeleteGlobalRef(g_last_world); g_last_world = nullptr; }
    if (world) g_last_world = env->NewGlobalRef(world);

    std::size_t dropped = 0;
    { std::lock_guard lk(g_cache_mx);       dropped = g_cache.size(); g_cache.clear(); }
    { std::lock_guard lk(g_pending_mx);     g_pending.clear(); }
    {
        std::lock_guard lk(g_chunk_queue_mx);
        g_chunk_queue.clear();
        // Must clear too, or chunks in the NEW world whose coords collide with
        // already-walked coords from the old one would be skipped as "recent".
        g_chunk_recent.clear();
    }

    blog("world change: ClientWorld identity changed — dropped %zu cached positions",
         dropped);

    // ARM a deferred re-seed rather than seeding right now. At this instant the
    // player entity has not spawned into the new world, so its position still
    // reads (0,0,0) and we would seed a spiral around the world origin —
    // walking hundreds of empty chunks, marking them "recently visited", and
    // never touching the chunks actually around the player. tick() retries
    // this until enqueue_loaded_chunks_impl reports success.
    if (world) {
        g_reseed_pending = true;
        g_reseed_at_ms   = GetTickCount64() + 500;   // let the spawn settle
    }

    if (world) env->DeleteLocalRef(world);
}

// Run the deferred re-seed, and self-heal if the cache is empty for a long
// time despite having targets selected. The self-heal covers the case where a
// seed landed while the player position was still wrong, or where all the
// server's ChunkData packets arrived before the module was ready — on a server
// those are delivered once at join, so without this nothing would ever
// re-trigger a scan.
void service_reseed(JNIEnv* env)
{
    const std::uint64_t now = GetTickCount64();

    if (g_reseed_pending && now >= g_reseed_at_ms) {
        if (enqueue_loaded_chunks_impl(env)) {
            g_reseed_pending = false;
            g_last_seed_ms   = now;
        } else {
            g_reseed_at_ms = now + 500;   // player still not ready; try again
        }
        return;
    }

    if (g_reseed_pending) return;   // waiting out the delay

    // TARGET-SET CHANGE — highest priority, and not rate limited. The user
    // just picked (or unpicked) a block and expects it on screen immediately.
    // Everything already walked sits in g_chunk_recent, so a plain re-seed
    // would skip it; clear that too and drop the old results.
    std::vector<std::string> target_ids_snap;
    { std::scoped_lock lk(g_settings_mx); target_ids_snap = g_settings.target_ids; }
    const std::size_t th = hash_targets(target_ids_snap);
    if (th != g_last_targets_hash) {
        g_last_targets_hash = th;
        std::size_t dropped = 0;
        { std::lock_guard lk(g_cache_mx); dropped = g_cache.size(); g_cache.clear(); }
        {
            std::lock_guard lk(g_chunk_queue_mx);
            g_chunk_queue.clear();
            g_chunk_recent.clear();
        }
        blog("reseed: target set changed (%zu targets) — dropped %zu, re-walking now",
             g_settings.target_ids.size(), dropped);

        // Report the colour each selected block resolved to, and via which
        // path. This is the direct answer to "why are all my ores the same
        // colour?" — if two targets print the same rgb from source=mapcolor,
        // they share a MapColor palette entry and need a curated rule.
        for (const auto& tid : g_settings.target_ids) {
            std::uint32_t rgb = 0;
            { std::lock_guard lk(g_reg_mx);
              auto it = g_reg_color.find(tid);
              if (it != g_reg_color.end()) rgb = it->second; }
            const char* src = curated_block_color(tid) ? "curated"
                            : (rgb == id_hash_color(tid) ? "hash" : "mapcolor");
            blog("  target '%s' -> #%06X (source=%s)",
                 tid.c_str(), rgb & 0xFFFFFFu, src);
        }
        if (!g_settings.target_ids.empty()) {
            g_last_seed_ms = now;
            enqueue_loaded_chunks_impl(env);
        }
        return;
    }

    if (g_settings.target_ids.empty()) return;

    // Track the player's chunk for both the movement test and the heartbeat.
    int pcx = 0, pcz = 0;
    if (!player_chunk(env, pcx, pcz)) return;
    g_dbg_player_cx = pcx;
    g_dbg_player_cz = pcz;

    std::size_t cq_n = 0;
    { std::lock_guard lk(g_chunk_queue_mx); cq_n = g_chunk_queue.size(); }
    if (cq_n != 0) return;              // still working through a seed

    // NEARBY REFRESH. Blocks you place or break yourself never reach any of
    // the three detection paths: the client predicts its own edits locally so
    // no BlockUpdate comes back, ChunkData only fires for newly-loaded chunks,
    // and the seed walk already marked these chunks as visited. The log showed
    // reveal_pkts=0 and chunkdata_pkts=0 for an entire creative session, which
    // is why new blocks only appeared after toggling the module off and on.
    //
    // Re-walk the chunks immediately around the player on a short interval and
    // REPLACE their cached contents, so placements appear and removals vanish.
    // Cost is bounded by the same 4 ms/tick budget the seed uses.
    if (now - g_last_refresh_ms >= 150) {
        g_last_refresh_ms = now;
        // 3x3 chunks (48 blocks) on a fast cadence. Breaking a block has to
        // clear its box promptly, and the only mechanism that notices a
        // REMOVAL is this re-walk — a removed block simply stops being
        // reported, so nothing else can infer it. Tightened from 5x5 @ 1000 ms
        // to 3x3 @ 250 ms: comparable chunks/second, four times the response.
        // Anything further out is picked up by the movement re-seed.
        constexpr int kRefreshRadius = 1;
        std::vector<pending_chunk> refresh;
        refresh.reserve((2 * kRefreshRadius + 1) * (2 * kRefreshRadius + 1));
        for (int dx = -kRefreshRadius; dx <= kRefreshRadius; ++dx)
            for (int dz = -kRefreshRadius; dz <= kRefreshRadius; ++dz)
                refresh.push_back({ pcx + dx, pcz + dz, 0, /*refresh=*/true });
        {
            std::lock_guard lk(g_chunk_queue_mx);
            // These coords are already in g_chunk_recent from the seed walk;
            // drop them so the drain doesn't coalesce the refresh away.
            for (const auto& pc : refresh) {
                g_chunk_recent.erase(((std::int64_t)pc.cx << 32) ^ (std::uint32_t)pc.cz);
            }
            g_chunk_queue.insert(g_chunk_queue.end(), refresh.begin(), refresh.end());
        }
        return;   // let the drain work through it before considering a re-seed
    }

    if (now - g_last_seed_ms < 1500) return;   // rate limit

    // MOVEMENT RE-SEED. A seed only covers a disc of g_seed_rc chunks around
    // where it was centred. Walk out of that disc and nothing ever re-triggers
    // a scan: the cache keeps the old area's blocks, they all fail the range
    // cull, and detection silently stops. That is exactly the reported
    // "detecta un momento y si me voy más lejos ya no detecta nada" — the log
    // showed cache=4873 with rng=0 for two minutes straight.
    //
    // Re-seed once the player is more than half the seed radius from its
    // centre, so coverage always overlaps and there is no dead band.
    const int dcx = pcx - g_seed_cx;
    const int dcz = pcz - g_seed_cz;
    const int moved = std::max(std::abs(dcx), std::abs(dcz));
    const int trigger = std::max(2, g_seed_rc / 2);

    std::size_t cache_n = 0;
    { std::lock_guard lk(g_cache_mx); cache_n = g_cache.size(); }

    const bool need_move_reseed = g_seed_valid && moved >= trigger;
    const bool need_empty_reseed = (cache_n == 0) && (now - g_last_seed_ms >= 5000);

    if (!need_move_reseed && !need_empty_reseed) return;

    if (need_move_reseed) {
        blog("reseed: player moved %d chunks from seed origin (%d,%d)->(%d,%d), "
             "trigger=%d — re-walking", moved, g_seed_cx, g_seed_cz, pcx, pcz, trigger);
        // Let the new area's chunks be walked again even if they were visited
        // under a previous seed.
        { std::lock_guard lk(g_chunk_queue_mx); g_chunk_recent.clear(); }
        prune_far_cache(pcx << 4, pcz << 4);
    } else {
        blog("reseed: cache empty with %zu targets — re-walking chunks around player",
             g_settings.target_ids.size());
    }

    g_last_seed_ms = now;             // back off regardless, avoids hot-looping
    enqueue_loaded_chunks_impl(env);
}

// ── Drain the chunk-load queue with a per-tick time budget ─────────────────
void drain_chunk_queue(JNIEnv* env)
{
    if (!C.mc_class || !C.mc_getInstance || !C.mc_world) return;

    std::unordered_set<std::string> targets;
    { std::scoped_lock lk(g_settings_mx); for (const auto& s : g_settings.target_ids) targets.insert(s); }
    if (targets.empty()) return;

    jobject client = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck() || !client) { env->ExceptionClear(); return; }
    jobject world = env->GetObjectField(client, C.mc_world);
    env->DeleteLocalRef(client);
    if (!world) return;

    // Pull batch under the lock, drop coalesced coords, respect ripen delay.
    std::vector<pending_chunk> batch;
    {
        std::lock_guard lk(g_chunk_queue_mx);
        const std::uint64_t now = GetTickCount64();
        for (auto it = g_chunk_queue.begin();
             it != g_chunk_queue.end() && batch.size() < 32; ) {
            const std::int64_t key = ((std::int64_t)it->cx << 32)
                                   ^ (std::uint32_t)it->cz;
            if (g_chunk_recent.count(key)) { it = g_chunk_queue.erase(it); continue; }
            // queued_ms=0 = "already loaded before we saw the packet, skip ripen"
            if (it->queued_ms != 0 && now - it->queued_ms < 50ull) { ++it; continue; }
            batch.push_back(*it);
            g_chunk_recent.insert(key);
            it = g_chunk_queue.erase(it);
        }
        if (g_chunk_recent.size() > 8192) g_chunk_recent.clear();
    }

    if (batch.empty()) { env->DeleteLocalRef(world); return; }

    // Time budget: checked between complete chunks; one chunk can overrun. A typical chunk walks in 200-800 μs so a
    // burst of ~10 chunks per tick fits comfortably; anything more rolls to
    // the next tick.
    vanta::util::TimeBudget budget(std::chrono::microseconds(4000));
    std::vector<std::pair<std::int64_t, std::uint32_t>> hits;
    hits.reserve(1024);
    // Chunks walked as a REFRESH. Their old cache contents must be dropped
    // before the new results go in, otherwise a block you broke keeps its box
    // forever — the walk simply wouldn't report it any more, and an add-only
    // merge never removes anything.
    std::unordered_set<std::int64_t> refreshed;
    std::size_t processed = 0;
    for (const auto& pc : batch) {
        if (budget.expired()) break;
        palette_walk_chunk(env, world, pc.cx, pc.cz, targets, hits);
        if (pc.refresh)
            refreshed.insert(((std::int64_t)pc.cx << 32) ^ (std::uint32_t)pc.cz);
        ++processed;
    }
    g_dbg_chunks_walked.fetch_add(static_cast<unsigned>(processed), std::memory_order_relaxed);
    g_dbg_walk_hits.fetch_add(static_cast<unsigned>(hits.size()), std::memory_order_relaxed);
    // Requeue any unprocessed coords (they were popped optimistically).
    if (processed < batch.size()) {
        std::lock_guard lk(g_chunk_queue_mx);
        for (std::size_t i = processed; i < batch.size(); ++i) {
            const std::int64_t key = ((std::int64_t)batch[i].cx << 32)
                                   ^ (std::uint32_t)batch[i].cz;
            g_chunk_recent.erase(key);
            g_chunk_queue.insert(g_chunk_queue.begin(), batch[i]);
        }
    }
    env->DeleteLocalRef(world);

    if (hits.empty() && refreshed.empty()) return;

    std::lock_guard lk(g_cache_mx);
    const std::uint64_t now = GetTickCount64();

    // One pass over the cache clearing every refreshed chunk, rather than a
    // scan per chunk — the refresh set is small but the cache can be tens of
    // thousands of entries.
    if (!refreshed.empty()) {
        for (auto it = g_cache.begin(); it != g_cache.end(); ) {
            int x, y, z; unpack_pos(it->first, x, y, z);
            const std::int64_t ck = ((std::int64_t)(x >> 4) << 32)
                                  ^ (std::uint32_t)(z >> 4);
            if (refreshed.count(ck)) it = g_cache.erase(it);
            else ++it;
        }
    }
    for (auto& [k, rgb] : hits) g_cache[k] = { rgb, now };
}

// Seed the queue with every chunk within Range around the player.
//
// Returns FALSE when the player isn't ready yet (no client, no world, no
// player entity) so the caller can retry on a later tick. That retry matters:
// calling this the instant a world change is detected seeds chunks around
// (0,0) because the player entity has not spawned and its position still
// reads as the origin — 289 empty chunks get walked, marked as recently
// visited, and the real chunks around the player are never scanned. On a
// server the ChunkData packets have already been delivered by then, so
// nothing ever re-triggers and the cache stays empty forever.
bool enqueue_loaded_chunks_impl(JNIEnv* env)
{
    if (!C.mc_class || !C.mc_getInstance || !C.mc_player || !C.ent_getX) return false;
    if (!C.mc_world) return false;

    jobject client = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck() || !client) { env->ExceptionClear(); return false; }

    // World must be live too — during a server transfer the player field can
    // briefly hold a stale entity while world is still null.
    jobject world = env->GetObjectField(client, C.mc_world);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!world) { env->DeleteLocalRef(client); return false; }
    env->DeleteLocalRef(world);

    jobject player = env->GetObjectField(client, C.mc_player);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!player) { env->DeleteLocalRef(client); return false; }

    const int px = (int)std::floor(env->CallDoubleMethod(player, C.ent_getX));
    const int pz = (int)std::floor(env->CallDoubleMethod(player, C.ent_getZ));
    const bool pos_ok = !env->ExceptionCheck();
    if (!pos_ok) env->ExceptionClear();
    env->DeleteLocalRef(player);
    env->DeleteLocalRef(client);
    if (!pos_ok) return false;

    const int R  = std::clamp(g_settings.range, 4, 128);
    const int rc = (R + 15) / 16;   // chunk radius
    const int pcx = px >> 4, pcz = pz >> 4;

    std::vector<pending_chunk> spiral;
    spiral.reserve((2 * rc + 1) * (2 * rc + 1));
    // Ring expansion so nearby chunks are walked first.
    for (int r = 0; r <= rc; ++r) {
        for (int dx = -r; dx <= r; ++dx) for (int dz = -r; dz <= r; ++dz) {
            if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
            spiral.push_back({ pcx + dx, pcz + dz, 0, false });
        }
    }
    {
        std::lock_guard lk(g_chunk_queue_mx);
        g_chunk_recent.clear();
        g_chunk_queue.insert(g_chunk_queue.begin(), spiral.begin(), spiral.end());
    }
    // Remember where this seed was centred so service_reseed can tell when the
    // player has walked out of the covered disc.
    g_seed_valid = true;
    g_seed_cx = pcx; g_seed_cz = pcz; g_seed_rc = rc;

    blog("enqueue_loaded_chunks: seeded %zu coords around (%d,%d) r=%d "
         "[player %d,%d]", spiral.size(), pcx, pcz, rc, px, pz);
    return true;
}

// Read the player's current chunk coords. Returns false when unavailable.
bool player_chunk(JNIEnv* env, int& out_cx, int& out_cz)
{
    if (!C.mc_class || !C.mc_getInstance || !C.mc_player || !C.ent_getX) return false;
    jobject client = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
    if (env->ExceptionCheck() || !client) { env->ExceptionClear(); return false; }
    jobject player = env->GetObjectField(client, C.mc_player);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!player) { env->DeleteLocalRef(client); return false; }
    const int px = (int)std::floor(env->CallDoubleMethod(player, C.ent_getX));
    const int pz = (int)std::floor(env->CallDoubleMethod(player, C.ent_getZ));
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    env->DeleteLocalRef(player);
    env->DeleteLocalRef(client);
    if (!ok) return false;
    out_cx = px >> 4; out_cz = pz >> 4;
    return true;
}

// Drop cached positions far outside the render range. Without this the cache
// keeps every block ever found while travelling: at 4873 entries and ~1000 fps
// that is 5 MILLION pointless iterations per second, all of them failing the
// range cull. Pruned at 2x range so nothing currently visible is ever lost.
void prune_far_cache(int px, int pz)
{
    const int R = std::clamp(g_settings.range, 4, 128);
    const double keep = static_cast<double>(R) * 2.0;
    const double keep2 = keep * keep;
    std::size_t before = 0, after = 0;
    {
        std::lock_guard lk(g_cache_mx);
        before = g_cache.size();
        for (auto it = g_cache.begin(); it != g_cache.end(); ) {
            int x, y, z; unpack_pos(it->first, x, y, z);
            const double dx = double(x) - px, dz = double(z) - pz;
            if (dx * dx + dz * dz > keep2) it = g_cache.erase(it);
            else ++it;
        }
        after = g_cache.size();
    }
    if (before != after)
        blog("prune: dropped %zu far cache entries (%zu -> %zu)",
             before - after, before, after);
}

void on_packet_receive(vanta::events::PacketReceiveEvent& e)
{
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!e.env || !e.packet) return;

    if (C.bu_pkt_class && e.env->IsInstanceOf(e.packet, C.bu_pkt_class) == JNI_TRUE) {
        handle_block_update(e.env, e.packet);
        return;
    }
    if (C.cd_pkt_class && e.env->IsInstanceOf(e.packet, C.cd_pkt_class) == JNI_TRUE) {
        handle_chunk_delta(e.env, e.packet);
        return;
    }
    // ChunkDataS2CPacket — the initial chunk envelope. We NEVER read the
    // WorldChunk here because vanilla hasn't installed it yet. Just capture
    // (cx,cz) for the drain to palette-walk on the game thread.
    if (C.cd_pkt2_class && C.cd_pkt2_chunkX && C.cd_pkt2_chunkZ &&
        e.env->IsInstanceOf(e.packet, C.cd_pkt2_class) == JNI_TRUE) {
        const jint cx = e.env->GetIntField(e.packet, C.cd_pkt2_chunkX);
        const jint cz = e.env->GetIntField(e.packet, C.cd_pkt2_chunkZ);
        if (!e.env->ExceptionCheck()) {
            g_dbg_chunkdata_pkts.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(g_chunk_queue_mx);
            g_chunk_queue.push_back({ (int)cx, (int)cz, GetTickCount64(), false });
            // Hard cap so a hostile server can't blow up memory. Drop oldest.
            if (g_chunk_queue.size() > 4096) g_chunk_queue.erase(g_chunk_queue.begin());
        }
        e.env->ExceptionClear();
        return;
    }
}

} // namespace

void set_focus_block(int x, int y, int z, bool enabled) {
    std::lock_guard lk(g_cache_mx);
    const auto key = pack_pos(x,y,z);
    if (enabled) {
        if (!g_focus_active || g_focus_key != key) {
            if (g_focus_active) {
                if (g_focus_previous) g_cache[g_focus_key] = *g_focus_previous;
                else g_cache.erase(g_focus_key);
            }
            g_focus_key = key;
            auto it = g_cache.find(key);
            g_focus_previous = it == g_cache.end() ? std::optional<cached_block>{} : std::optional<cached_block>{it->second};
            g_cache[key] = {kFocusRgb, GetTickCount64()};
            g_focus_active = true;
        } else {
            g_cache[key] = {kFocusRgb, GetTickCount64()};
        }
    } else if (g_focus_active) {
        if (g_focus_previous) g_cache[g_focus_key] = *g_focus_previous;
        else g_cache.erase(g_focus_key);
        g_focus_previous.reset();
        g_focus_active = false;
    }
}

// ── Public API ──────────────────────────────────────────────────────────────
static std::unique_ptr<vanta::IListenerToken> packet_listener;

void install()
{
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    vanta::vgui::bridge::setBlockPickerProvider([]() -> const std::vector<vanta::vgui::bridge::BlockPickerRow>& {
        std::lock_guard lk(g_reg_mx);
        return g_reg_rows;
    });

    // Retain the subscription: this EventBus unsubscribes when its token dies.
    packet_listener = vanta::EventBus::get().subscribe<vanta::events::PacketReceiveEvent>(
        [](vanta::events::PacketReceiveEvent& e) { on_packet_receive(e); },
        vanta::Priority::Monitor);

    // NOTE: there is deliberately NO events::WorldChangeEvent subscription
    // here. That event is declared in core/event/Events.h but is never posted
    // by anything — src/hook/sites/ has no WorldChangeSite — so subscribing to
    // it was dead code that merely LOOKED like it handled server changes,
    // while the cache actually survived across servers and drew boxes for
    // blocks that were never there. World changes are detected instead by
    // comparing ClientWorld object identity in detect_world_change(), called
    // from tick().

    // Camera capture — shared with hitfx / esp / chams; enabling it once is
    // idempotent because the underlying counter is atomic on their side.
    cap::set_capture_enabled(true);

    VLOG_INFO("blockesp", "installed (provider + PacketReceive listener + camera capture)");
    blog("install: provider registered, PacketReceive subscribed, camera capture ON");
}

void shutdown()
{
    g_enabled.store(false);
    packet_listener.reset();
    vanta::vgui::bridge::setBlockPickerProvider(nullptr);
    // Cache + registry stay allocated so a re-enable is instant. The
    // vgui provider callback also stays — it's read every frame regardless
    // of module state, and returning an empty list is the correct behaviour
    // when the engine hasn't populated it yet.
    if (g_last_world) {
        ::vanta::jvm::JniScope s;
        if (s) s.get()->DeleteGlobalRef(g_last_world);
        g_last_world = nullptr;
    }
    g_installed.store(false, std::memory_order_release);
}

void enqueue_loaded_chunks()
{
    ::vanta::jvm::JniScope s;
    if (!s) return;
    // If the player isn't ready (enabled from the menu, or mid server
    // transfer), arm the deferred retry instead of silently doing nothing.
    // Anchor the target hash here so the first tick after enabling doesn't
    // see a "changed" set and immediately re-walk what we just seeded.
    { std::scoped_lock lk(g_settings_mx); g_last_targets_hash = hash_targets(g_settings.target_ids); }

    if (enqueue_loaded_chunks_impl(s.get())) {
        g_last_seed_ms   = GetTickCount64();
        // Cancel any armed retry — a world change while disabled can leave one
        // pending, which then fires right after this and seeds a second time
        // (the log showed two identical seeds 31 ms apart, double-queueing 98
        // coords for 49 chunks).
        g_reseed_pending = false;
    } else {
        g_reseed_pending = true;
        g_reseed_at_ms   = GetTickCount64() + 500;
    }
}

void tick(void* env_opaque)
{
    JNIEnv* env = static_cast<JNIEnv*>(env_opaque);
    if (!env) return;

    resolve_all(env);

    // Heartbeat — every ~2s log the current resolve status so any silent JNI
    // failure surfaces without needing to reproduce it. Rate-limited so the
    // log file stays readable.
    static std::atomic<unsigned long long> s_last_hb{0};
    const unsigned long long now = GetTickCount64();
    if (now - s_last_hb.load(std::memory_order_relaxed) > 2000) {
        s_last_hb.store(now, std::memory_order_relaxed);
        std::size_t reg_n = 0, cache_n = 0, cq_n = 0;
        { std::lock_guard lk(g_reg_mx);         reg_n   = g_reg_rows.size(); }
        { std::lock_guard lk(g_cache_mx);       cache_n = g_cache.size();    }
        { std::lock_guard lk(g_chunk_queue_mx); cq_n    = g_chunk_queue.size(); }
        const bool palette_ready = C.world_getChunk && C.chunk_getSectionArray
                                && C.section_getContainer && C.pc_data_field
                                && C.pcdata_palette && C.pcdata_storage
                                && C.palette_get && C.palette_getSize
                                && C.pstor_writeIndices && C.pstor_scratch;
        // Read + RESET render counters. These accumulate across ~2s of frames;
        // the log shows the ratios so the pipeline stage that dropped the
        // "invisible from certain angles" case is obvious.
        const unsigned frames  = g_dbg_render_frames.exchange(0, std::memory_order_relaxed);
        const unsigned seen    = g_dbg_cache_seen.exchange(0, std::memory_order_relaxed);
        const unsigned rng_ok  = g_dbg_pass_range.exchange(0, std::memory_order_relaxed);
        const unsigned fwd_ok  = g_dbg_pass_forward.exchange(0, std::memory_order_relaxed);
        const unsigned vis_ok  = g_dbg_pass_visible.exchange(0, std::memory_order_relaxed);
        const unsigned edges   = g_dbg_edges_drawn.exchange(0, std::memory_order_relaxed);
        const unsigned faces   = g_dbg_faces_drawn.exchange(0, std::memory_order_relaxed);
        const unsigned cd_pkts = g_dbg_chunkdata_pkts.exchange(0, std::memory_order_relaxed);
        const unsigned walked  = g_dbg_chunks_walked.exchange(0, std::memory_order_relaxed);
        const unsigned whits   = g_dbg_walk_hits.exchange(0, std::memory_order_relaxed);
        const unsigned rvpkts  = g_dbg_reveal_pkts.exchange(0, std::memory_order_relaxed);
        blog("detect: player_chunk=(%d,%d) seed=(%d,%d) r=%d moved=%d | "
             "chunkdata_pkts=%u chunks_walked=%u walk_hits=%u reveal_pkts=%u",
             g_dbg_player_cx, g_dbg_player_cz,
             g_seed_cx, g_seed_cz, g_seed_rc,
             g_seed_valid ? std::max(std::abs(g_dbg_player_cx - g_seed_cx),
                                     std::abs(g_dbg_player_cz - g_seed_cz)) : -1,
             cd_pkts, walked, whits, rvpkts);
        blog("tick: enabled=%d reg_ready=%d reg_rows=%zu cache=%zu targets=%zu cq=%zu | "
             "render frames=%u seen=%u rng=%u fwd=%u vis=%u edges=%u faces=%u partial=%u | "
             "resolve: mc=%s world=%s bp=%s bs=%s block=%s mapcolor_m=%s mapcolor_f=%s "
             "regs=%s regs.BLOCK=%s registry=%s ident=%s "
             "pkt.BlockUpdate=%s pkt.ChunkDelta=%s pkt.ChunkData=%s "
             "palette=%s",
             (int)g_enabled.load(std::memory_order_relaxed),
             (int)g_registry_ready.load(std::memory_order_acquire),
             reg_n, cache_n, g_settings.target_ids.size(), cq_n,
             frames, seen, rng_ok, fwd_ok, vis_ok, edges, faces,
             g_dbg_partial_edges.exchange(0, std::memory_order_relaxed),
             C.mc_class        ? "ok" : "MISSING",
             C.world_getBlockState ? "ok" : "MISSING",
             C.bp_ctor         ? "ok" : "MISSING",
             C.bs_getBlock     ? "ok" : "MISSING",
             C.block_class     ? "ok" : "MISSING",
             C.block_getDefaultMapColor ? "ok" : "MISSING",
             C.mc_color_field ? "ok" : "MISSING",
             C.registries_class ? "ok" : "MISSING",
             C.registries_block ? "ok" : "MISSING",
             C.registry_iterator ? "ok" : "MISSING",
             C.id_toString     ? "ok" : "MISSING",
             (C.bu_pkt_class && (C.bu_pkt_pos || C.bu_pkt_getPos)
                              && (C.bu_pkt_state || C.bu_pkt_getState)) ? "ok" : "MISSING",
             (C.cd_pkt_class && C.cd_pkt_positions && C.cd_pkt_states) ? "ok" : "MISSING",
             (C.cd_pkt2_class && C.cd_pkt2_chunkX && C.cd_pkt2_chunkZ) ? "ok" : "MISSING",
             palette_ready ? "ok" : "MISSING");
    }

    // Registry enumeration retried until the classloader has Registries.BLOCK
    // populated (usually mid-title-screen, always before joining a world).
    if (!g_registry_ready.load(std::memory_order_acquire))
        enumerate_registry(env);

    // Drop the cache when the world identity changes (server hop, dimension
    // change, disconnect). Runs even while disabled so re-enabling never shows
    // positions from a previous world.
    detect_world_change(env);

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Live scan is gated on both the anti-xray mode AND the explicit
    // use_live_scan toggle (default OFF). With packets working, the scanner
    // is redundant for the majority of gameplay and the per-position JNI
    // cost dominates FPS.
    if (g_settings.use_live_scan) {
        static std::atomic<unsigned long long> s_last_scan{0};
        const unsigned long long tnow = GetTickCount64();
        if (tnow - s_last_scan.load(std::memory_order_relaxed) >= 100ull) {
            s_last_scan.store(tnow, std::memory_order_relaxed);
            live_scan(env);
        }
    }

    // Deferred re-seed after a world change, plus the empty-cache self-heal.
    service_reseed(env);

    drain_reveals();
    drain_chunk_queue(env);   // palette-walk newly-loaded / spiral-seeded chunks
}

// ── Render ─────────────────────────────────────────────────────────────────
void render_text(int display_w, int display_h)
{
    if (!g_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lk(g_cache_mx);
        if (!g_focus_active) return;
    }
    // Same degenerate-size guard EspModule uses (esp.cpp:1610). Only possible
    // now that the framebuffer dimensions are no longer discarded — see the
    // clip-rect note below for why throwing them away was the whole bug.
    if (display_w < 2 || display_h < 2) return;

    // Arm the shared camera capture service EVERY frame from here — set_capture_enabled
    // is a counter, and capture_now is the pump. Without both, latest_frame stays
    // valid=0 for the entire session unless another render module (chams / hitfx /
    // esp) happens to also be on. See chams.cpp:220-244 for the same pattern.
    // Snapshot camera + settings colour choice.
    hf::render_frame f{};
    {
        ::vanta::jvm::JniScope s;
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

        // THE fix for "el highlight no funciona bien" while moving. The
        // captured frame is the camera rotation plus the basic projection, and
        // neither carries GameRenderer.bobView / tiltViewWhenHurt — the walking
        // sway and the damage roll the world is actually drawn through.
        // Projected without them the boxes are exact while standing still and
        // swing off the blocks with every step (a few degrees of view rotation,
        // i.e. tens of pixels at any range) and on every hit taken. The
        // PlayerESP has always folded that matrix in (esp.cpp build_proj); this
        // is the same correction, shared (see world_overlay_view.h).
        overlay_view::apply_view_bob(env, f);
    }

    // Player center — for range culling against cached entries. Use the
    // camera position (close enough for a render-side range test).
    const double cx = f.cam_x, cy = f.cam_y, cz = f.cam_z;
    const int    R  = std::clamp(g_settings.range, 4, 128);
    // Range cull is squared-distance vs squared radius. The earlier build had
    // `> R2 * R2` (R^4), which let blocks at 4× the intended radius through —
    // ClearCache looked broken because "distant" entries kept redrawing.
    const double R2 = static_cast<double>(R) * static_cast<double>(R);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    WorldOverlayDrawScope draw_scope(dl);

    // THE fix for "el blockesp solo funciona en una posición de mi cámara".
    //
    // gl_hook.cc:658 deliberately pins io.DisplaySize to the ClickGui CANVAS
    // (k_window_width x k_window_height = 1120x700) before ImGui::NewFrame,
    // so the panel's own clip rects survive in canvas units. ImGui then seeds
    // the shared foreground draw list every frame with
    // PushClipRect((0,0),(1120,700)). gl_hook skips the "##Foreground" list
    // when it rescales clip rects, and at :838 restores dd->DisplaySize to the
    // real framebuffer — so that stale 1120x700 canvas rect reaches the GL
    // backend verbatim as a glScissor in FRAMEBUFFER pixels.
    //
    // Our coordinates are framebuffer pixels. On any window larger than
    // 1120x700, everything outside the top-left corner was being thrown away
    // by the GPU: boxes appeared only while the camera happened to place them
    // up and to the left, and vanished otherwise.
    //
    // intersect=false REPLACES the inherited rect rather than intersecting
    // with it. Every other foreground module already does this (esp.cpp:1456,
    // hitfx, arraylist, watermark, spotify, targethud, radar, notifier);
    // BlockESP was the only one missing it, because its signature discarded
    // display_w/display_h and so there was nothing to push.
    //
    // This is also why four upstream fixes (forward cull, near-plane clip,
    // NDC-z opt-out, sanity clamp) all failed: the geometry was never wrong.
    // The counters increment right after AddLine/AddConvexPolyFilled, but
    // scissoring happens far downstream inside the GL backend — so every
    // counter could read perfectly healthy (edges/vis = 12.0, faces/vis = 6.0)
    // while nothing reached the screen.
    dl->PushClipRect(ImVec2(0.f, 0.f),
                     ImVec2(static_cast<float>(display_w),
                            static_cast<float>(display_h)),
                     false);

    // Force the GEOMETRY path for anti-aliased lines.
    //
    // imgui_draw.cpp:817 picks a texture-sampling path instead whenever
    //     AntiAliasedLinesUseTex && fractional_thickness <= 0.00001f
    // i.e. at INTEGER thicknesses — exactly the values LineWidth lands on.
    // That path samples _Data->TexUvLines from whatever texture is currently
    // bound on the draw list, and this foreground list is shared: arraylist,
    // esp, hitfx, notifier, radar and spotify all PushTextureID their own
    // atlases and album art onto it. Our line quads then sample somebody
    // else's texture and rasterize as garbage streaks — which is why the
    // artefact tracked LineWidth and never showed up in the geometry
    // counters (`partial=0` in every sample while the boxes still looked
    // wrong).
    //
    // Clearing the flag keeps anti-aliasing, it just builds the fringe as
    // geometry instead of sampling. A few more vertices per line; nothing
    // measurable at these counts.
    const ImDrawListFlags saved_flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;

    // Reset per-frame diagnostics (cumulative sums; heartbeat divides by frames).
    g_dbg_render_frames.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lk(g_cache_mx);
    for (const auto& [key, cb] : g_cache) {
        g_dbg_cache_seen.fetch_add(1, std::memory_order_relaxed);
        int x, y, z;
        unpack_pos(key, x, y, z);

        // Range cull (in blocks, camera-relative).
        const double dx = (x + 0.5) - cx;
        const double dy = (y + 0.5) - cy;
        const double dz = (z + 0.5) - cz;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > R2) continue;
        g_dbg_pass_range.fetch_add(1, std::memory_order_relaxed);

        // NO forward-dot cull.
        //
        // Three successive versions of a camera-forward pre-cull (raw dot < 0,
        // normalized dot < -0.90, raw dot < -0.87) all produced the same user
        // report: boxes vanish wholesale at certain view angles. The render
        // counters caught it red-handed — one 2 s window logged
        // `rng=51084 fwd=0 vis=0 edges=0`, i.e. every block in range was
        // thrown away by this stage alone while projection was never even
        // attempted.
        //
        // The forward vector is derived from f.modelview's third row, but that
        // matrix is built as RotX(pitch)·RotY(yaw+180) with MC's own sign
        // conventions baked in, so treating -row2 as "world-space forward" is
        // not reliable here. EspModule — which renders correctly at every
        // angle — has no equivalent cull: its project() is the same math as
        // hf::world_to_screen and it gives up only when all 8 corners fail
        // (nvis == 0), then skips on a 2D screen-bbox test AFTER projection.
        //
        // Projection is the authority on visibility. Eight world_to_screen
        // calls per cached block is trivial at realistic cache sizes, and the
        // cull was saving nothing measurable. Counter kept, wired to the same
        // value as rng, so the log's column layout stays stable across builds.
        g_dbg_pass_forward.fetch_add(1, std::memory_order_relaxed);

        // Colour is always the block's own, resolved at registry-enumeration
        // time (curated mineral colour -> MapColor -> identifier hash). Wire
        // lines are fully opaque; FillAlpha controls the faces separately.
        const std::uint32_t rgb = cb.rgb ? cb.rgb : kUnknownBlockRgb;
        const bool anchor_focus = g_focus_active && key == g_focus_key;
        const ImU32 col = IM_COL32((rgb >> 16) & 0xff,
                                   (rgb >> 8)  & 0xff,
                                    rgb        & 0xff,
                                   255);

        // Project all 8 AABB corners via hf::world_to_screen (which mirrors
        // what ESP uses and has no known bugs). Track ok/coords per corner.
        //
        // The earlier build's custom view-space clipping had a subtle bug that
        // made boxes vanish at random camera angles. Instead of re-doing the
        // clip in view space, we use hf projection everywhere and RECOVER
        // partial-visibility edges via world-space bisection: if one endpoint
        // is behind camera (projection fails) but the other is visible, walk
        // the edge from the visible endpoint toward the failed one, halving
        // the step each iteration, until we hit the last point that still
        // projects. Six iterations gives ~1.5% edge-length precision, which
        // is invisible.
        struct pt { float sx, sy; bool ok; double wx, wy, wz; };
        pt p[8];
        static const int cx_off[8] = { 0, 1, 1, 0, 0, 1, 1, 0 };
        static const int cy_off[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
        static const int cz_off[8] = { 0, 0, 1, 1, 0, 0, 1, 1 };
        // Sanity-clamp for near-eye corners. hf::world_to_screen returns TRUE
        // whenever clip_w > 0.0001, but a corner right at the near plane has
        // clip_w ≈ 0.001 and NDC = clip / clip_w explodes to millions of
        // pixels — the line gets drawn OFF SCREEN and the user sees nothing
        // (that's the "fov" bug: the counters showed edges being emitted
        // that never landed inside the viewport). Reject anything projected
        // more than 5 viewports out and let the bisection recover the
        // near-plane crossing instead.
        const float vw = static_cast<float>(f.viewport_w);
        const float vh = static_cast<float>(f.viewport_h);
        // How far outside the viewport a projected corner may land before we
        // treat it as unusable. A corner sitting between the eye plane and the
        // near plane has clip_w ~= 0, so NDC = clip/clip_w explodes and the
        // point lands hundreds of screens away. Anything that survives here
        // becomes a hull vertex, and a single runaway vertex stretches the
        // silhouette into a huge spike shooting off the block — the "salidas".
        //
        // 1.5 viewports is enough slack for a legitimately off-screen corner of
        // a block you are standing inside, while keeping every drawn vertex
        // close enough that the bisection lands on the visible boundary.
        const float sane_x = vw * 1.5f;
        const float sane_y = vh * 1.5f;

        int visible = 0;
        for (int i = 0; i < 8; ++i) {
            p[i].wx = x + cx_off[i];
            p[i].wy = y + cy_off[i];
            p[i].wz = z + cz_off[i];
            float depth = 0.f;
            p[i].ok = hf::world_to_screen(f, p[i].wx, p[i].wy, p[i].wz,
                                          p[i].sx, p[i].sy, depth,
                                          /*clip_z=*/false);
            if (p[i].ok) {
                if (std::fabs(p[i].sx) > sane_x || std::fabs(p[i].sy) > sane_y)
                    p[i].ok = false;
                else
                    ++visible;
            }
        }
        if (visible == 0) continue;

        // Off-screen skip, done on ALREADY-PROJECTED coordinates. This is the
        // optimisation that replaces the deleted camera-forward cull: it uses
        // the real projected extent instead of guessing from a dot product, so
        // it cannot be wrong about what is visible. Same test EspModule uses.
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (int i = 0; i < 8; ++i) {
            if (!p[i].ok) continue;
            bx0 = std::min(bx0, p[i].sx); by0 = std::min(by0, p[i].sy);
            bx1 = std::max(bx1, p[i].sx); by1 = std::max(by1, p[i].sy);
        }
        if (bx1 < 0.f || by1 < 0.f || bx0 > vw || by0 > vh) continue;

        g_dbg_pass_visible.fetch_add(1, std::memory_order_relaxed);

        // Walk from a projectable point toward a failed one, bisecting on
        // failure until we find the last projectable point. Hoisted ABOVE
        // the fill loop so face recovery can reuse it.
        auto bisect_to = [&, sane_x, sane_y](double src_wx, double src_wy, double src_wz,
                             float src_sx, float src_sy,
                             double dst_wx, double dst_wy, double dst_wz,
                             float& out_sx, float& out_sy) {
            double good_wx = src_wx, good_wy = src_wy, good_wz = src_wz;
            float  good_sx = src_sx, good_sy = src_sy;
            double lo = 0.0, hi = 1.0;
            for (int step = 0; step < 8; ++step) {
                const double t = (lo + hi) * 0.5;
                const double mx = src_wx + t * (dst_wx - src_wx);
                const double my = src_wy + t * (dst_wy - src_wy);
                const double mz = src_wz + t * (dst_wz - src_wz);
                float sx, sy, d;
                // Same sanity clamp as the corner projection above — treat a
                // near-eye midpoint (whose sx/sy exploded) as "failed" so
                // the bisection walks back into the visible region rather
                // than converging on off-screen infinity.
                const bool proj_ok = hf::world_to_screen(f, mx, my, mz, sx, sy, d,
                                                         /*clip_z=*/false)
                                  && std::fabs(sx) <= sane_x
                                  && std::fabs(sy) <= sane_y;
                if (proj_ok) {
                    good_wx = mx; good_wy = my; good_wz = mz;
                    good_sx = sx; good_sy = sy;
                    lo = t;
                } else {
                    hi = t;
                }
            }
            out_sx = good_sx;
            out_sy = good_sy;
        };

        // 12 edges of a cube.
        static const int edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},     // bottom
            {4,5},{5,6},{6,7},{7,4},     // top
            {0,4},{1,5},{2,6},{3,7},     // verticals
        };

        // Boundary-recovery points, computed ONCE and shared by the fill and
        // the wires. When one end of an edge is unusable, bisection walks from
        // the visible end toward it and stops at the last point that still
        // projects sanely — i.e. on the frustum boundary. Feeding these same
        // points into both the hull and the lines is what keeps the fill and
        // the outline exactly coincident; deriving them separately let the
        // wires reach past the fill and read as spikes.
        struct recpt { float sx, sy; bool ok; };
        recpt rec[12];
        for (int ei = 0; ei < 12; ++ei) {
            rec[ei].ok = false;
            const pt& ra = p[edges[ei][0]];
            const pt& rb = p[edges[ei][1]];
            if (ra.ok == rb.ok) continue;          // both usable, or neither
            const pt& src = ra.ok ? ra : rb;
            const pt& dst = ra.ok ? rb : ra;
            bisect_to(src.wx, src.wy, src.wz, src.sx, src.sy,
                      dst.wx, dst.wy, dst.wz, rec[ei].sx, rec[ei].sy);
            rec[ei].ok = true;
        }

        // ── Fill: ONE silhouette polygon, not six faces ───────────────────
        //
        // The obvious implementation draws the cube's 6 faces as separate
        // translucent quads. That looks wrong and it is what produced the
        // "líneas feas" on every block:
        //
        //   * Every pixel of the cube gets covered TWICE — you always see a
        //     front face and a back face through it — so the alpha blends
        //     twice and saturates well before FillAlpha reaches 1.
        //   * The 6 projected quads meet along 3 internal diagonals. ImGui
        //     feathers the edge of each anti-aliased fill, so two feathered
        //     edges overlap along every seam and draw a visible line right
        //     across the block face.
        //
        // A cube's silhouette is a convex polygon (a hexagon in general, a
        // quad when viewed face-on), so fill the 2D CONVEX HULL of the
        // projected corners exactly once. Uniform coverage, exact alpha, and
        // no internal seams at any angle.
        const float fill_alpha = g_settings.fill_alpha;
        if (fill_alpha > 0.001f) {
            // Usable corners PLUS the frustum-boundary points, so the filled
            // silhouette ends exactly where the outline does.
            ImVec2 pts[20];
            int n = 0;
            for (int i = 0; i < 8; ++i)
                if (p[i].ok) pts[n++] = ImVec2(p[i].sx, p[i].sy);
            for (int ei = 0; ei < 12; ++ei)
                if (rec[ei].ok) pts[n++] = ImVec2(rec[ei].sx, rec[ei].sy);

            // Collapse near-identical points BEFORE building the hull.
            // Viewed close to face-on, four corners of the cube project within
            // a fraction of a pixel of the other four. Feeding those to the
            // hull leaves near-duplicate consecutive vertices, and
            // AddConvexPolyFilled fan-triangulates them into degenerate
            // slivers that rasterize as stray hairlines across the block.
            {
                int m = 0;
                for (int i = 0; i < n; ++i) {
                    bool dup = false;
                    for (int j = 0; j < m; ++j) {
                        if (std::fabs(pts[i].x - pts[j].x) < 0.5f &&
                            std::fabs(pts[i].y - pts[j].y) < 0.5f) { dup = true; break; }
                    }
                    if (!dup) pts[m++] = pts[i];
                }
                n = m;
            }

            if (n >= 3) {
                // Andrew's monotone chain. At most 20 inputs, so the sort and
                // the two passes are trivial next to the 8 projections above.
                std::sort(pts, pts + n, [](const ImVec2& a, const ImVec2& b) {
                    return a.x < b.x || (a.x == b.x && a.y < b.y);
                });
                auto cross = [](const ImVec2& o, const ImVec2& a, const ImVec2& b) {
                    return (double)(a.x - o.x) * (double)(b.y - o.y)
                         - (double)(a.y - o.y) * (double)(b.x - o.x);
                };
                ImVec2 hull[42];
                int k = 0;
                for (int i = 0; i < n; ++i) {
                    while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
                    hull[k++] = pts[i];
                }
                const int lower = k + 1;
                for (int i = n - 2; i >= 0; --i) {
                    while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
                    hull[k++] = pts[i];
                }
                --k;   // last point equals the first

                // Reject a degenerate silhouette. A block seen exactly
                // edge-on collapses to a sliver whose triangles are thinner
                // than a pixel; rasterized, that is a stray streak rather
                // than a shape. The wire still draws it correctly.
                double area2 = 0.0;
                for (int i = 0, j = k - 1; i < k; j = i++) {
                    area2 += (double)hull[j].x * hull[i].y
                           - (double)hull[i].x * hull[j].y;
                }
                if (k >= 3 && std::fabs(area2) * 0.5 >= 1.0) {
                    const int a_channel =
                        std::clamp(static_cast<int>(fill_alpha * 255.f), 0, 255);
                    const ImU32 fcol =
                        (col & 0x00FFFFFFu) | (static_cast<ImU32>(a_channel) << 24);

                    // Keep anti-aliasing ON. Disabling it to stop the fill's
                    // feather doubling the wire traded one artefact for a
                    // worse one: a hard-edged fill stair-steps along every
                    // diagonal silhouette edge, and those jaggies poke out
                    // from behind the smooth anti-aliased wire.
                    dl->AddConvexPolyFilled(hull, k, fcol);

                    g_dbg_faces_drawn.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // ── Wire edges ────────────────────────────────────────────────────
        //
        // Twelve independent AddLine calls, deliberately. An earlier build drew
        // the two horizontal rings as closed AddPolylines to get mitred corner
        // joins, and that is what produced the lines shooting out of the box:
        // ImGui miters polyline joins with NO miter limit, so when two adjacent
        // segments meet at a sharp angle the join point is extrapolated far
        // outside the shape. A cube's top and bottom faces are heavily
        // foreshortened whenever you look at it from above or below, which
        // makes exactly those angles sharp — hence spikes on almost every box.
        //
        // Separate segments just overlap at the corners instead. The colour is
        // fully opaque, so overlapping is invisible, and there is nothing to
        // extrapolate.
        // Fixed one-pixel outline. The width used to be user-configurable and
        // every value above 1 caused trouble: on a block only a few dozen
        // pixels across, twelve strokes several pixels thick overlap into a
        // blob, and any integer width sends ImGui down its texture-sampling
        // line path. A hairline reads as a clean edge on the filled silhouette
        // at every distance, so FillAlpha is the only thing left to tune.
        constexpr float lw = 1.0f;
        auto draw_edge = [&](ImVec2 from, ImVec2 to) {
            // A small halo only on the placed anchor. Independent segments
            // preserve the clipping above without sharp polyline miter spikes.
            if (anchor_focus) {
                dl->AddLine(from, to, IM_COL32(255, 194, 70, 12), 7.f);
                dl->AddLine(from, to, IM_COL32(255, 206, 95, 25), 4.5f);
                dl->AddLine(from, to, IM_COL32(255, 224, 140, 50), 2.5f);
                dl->AddLine(from, to, IM_COL32(255, 239, 185, 255), 1.2f);
            } else {
                dl->AddLine(from, to, col, lw);
            }
        };

        for (int ei = 0; ei < 12; ++ei) {
            const pt& a = p[edges[ei][0]];
            const pt& b = p[edges[ei][1]];

            if (a.ok && b.ok) {
                // Skip edges that collapsed to a point. Seen close to face-on
                // the four depth edges project to near-zero length, and a
                // thick anti-aliased stroke over a degenerate segment
                // rasterizes as a blob at the corner rather than a line —
                // the same class of artefact the degenerate hull triangles
                // produced in the fill.
                const float ex = b.sx - a.sx, ey = b.sy - a.sy;
                if (ex * ex + ey * ey < 0.25f) continue;

                draw_edge(ImVec2(a.sx, a.sy), ImVec2(b.sx, b.sy));
                g_dbg_edges_drawn.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!a.ok && !b.ok) continue;

            // Partial edge: stop at the shared frustum-boundary point so the
            // outline ends exactly where the filled silhouette does.
            if (!rec[ei].ok) continue;
            const pt& vis = a.ok ? a : b;
            // The recovery point must land essentially on screen. Bisection
            // stops at the last position that projected within the sanity
            // bound, and that bound is 1.5 viewports — far enough that a
            // partial edge could still streak well outside the picture.
            if (rec[ei].sx < -vw * 0.05f || rec[ei].sx > vw * 1.05f ||
                rec[ei].sy < -vh * 0.05f || rec[ei].sy > vh * 1.05f) continue;
            const float px_ = rec[ei].sx - vis.sx, py_ = rec[ei].sy - vis.sy;
            if (px_ * px_ + py_ * py_ < 0.25f) continue;   // collapsed, skip
            g_dbg_partial_edges.fetch_add(1, std::memory_order_relaxed);
            draw_edge(ImVec2(vis.sx, vis.sy), ImVec2(rec[ei].sx, rec[ei].sy));
            g_dbg_edges_drawn.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Balanced with the PushClipRect above. Both early returns sit above the
    // push, and the cache loop only ever uses `continue`, so this is always
    // reached exactly once per push.
    dl->Flags = saved_flags;
    dl->PopClipRect();
}

} // namespace vanta::modules::blockesp

