// blockesp.h — engine surface for the BlockESP module.
//
// The engine does four things:
//   1. Enumerates Registries.BLOCK once the world exists and publishes a
//      snapshot (id + name + map colour) to the ClickGui block-picker widget
//      through vgui::bridge::setBlockPickerProvider.
//   2. Runs a throttled scan over loaded chunks around the player, collecting
//      the world-position of every block whose identifier is in the user's
//      selected set. Cache is thread-safe.
//   3. Listens on PacketReceiveEvent for BlockUpdate / ChunkDelta /
//      ChunkData packets and folds the revealed positions into the same
//      cache. This is what makes the module anti-xray resistant: any block
//      the server ever sends (including an ore leaked by a broken neighbour)
//      lands in the cache and stays there, independent of what MC's own
//      world data currently says.
//   4. Draws colour-per-block wire boxes on the ImGui foreground drawlist
//      each frame, using the shared hitfx camera capture for world→screen.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vanta::modules::blockesp {

struct settings {
    int   range          = 32;         // scan + render radius, blocks
    float fill_alpha     = 0.25f;      // 0 = wireframe only; >0 = filled faces at this alpha
    bool  show_through_walls = true;   // always true for this pipeline; kept for symmetry
    // Live scan is EXPENSIVE (per-position getBlockState triple-loop). With
    // the packet-reveal path working, most gameplay only needs packets —
    // scan should be off by default. Turn on only if you need immediate
    // detection of blocks you've never had a BlockUpdate/ChunkDelta for.
    bool  use_live_scan  = false;
    // The user's picked identifiers; mirrored by the module every frame. Only
    // touched from the render/game thread, never from netty (drain queues into
    // a game-thread-only consumer).
    std::vector<std::string> target_ids;
};

// Colour is ALWAYS the block's own — curated mineral colour first, then
// MapColor, then an identifier hash. There is no custom-colour mode: a single
// user-picked colour made every selected block look identical, which defeats
// the point of picking several. Wire lines draw fully opaque; FillAlpha
// controls the faces.
inline constexpr std::uint32_t kUnknownBlockRgb = 0x9E9E9Eu;

extern settings          g_settings;
extern std::mutex        g_settings_mx;
// Toggled from the module thread (onEnable/onDisable), read every frame from
// the render thread AND from the netty packet thread — needs to be atomic to
// avoid tearing / compiler-CSE hiding a toggle.
extern std::atomic<bool> g_enabled;
// Desktop debug log gate. Mirrored from the module's "Debug" setting. When
// false nothing is logged and no file is created — off at inject by default.
extern std::atomic<bool> g_debug;

// Called from the render pass — same slot as chams::render_text / esp::render_text
// so all screen-space overlays live on the same drawlist and blend order.
void render_text(int display_w, int display_h);

// Transient combat focus overlay. It is independent from the BlockESP toggle
// and is used by anchor modules to show the currently selected block.
void set_focus_block(int x, int y, int z, bool enabled);

// Called on every GameLoopEvent from BlockEspModule. Runs the periodic scan +
// drains the packet-fed reveal queue. Safe when a world isn't loaded (no-op).
void tick(void* env_opaque);

// Register the packet listener + provider seams. Idempotent. Called from the
// module's onInit.
void install();
void shutdown();

// Seed the chunk-walk queue with every chunk in a spiral around the player,
// bounded by settings.range. Called from onEnable so the module immediately
// covers the currently-loaded world without waiting for ChunkData packets.
void enqueue_loaded_chunks();

} // namespace vanta::modules::blockesp
