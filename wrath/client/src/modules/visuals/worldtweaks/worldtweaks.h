// worldtweaks.h — engine surface for the WorldTweaks module.
//
// WorldTweaks changes what the LOCAL client renders for the world's time of
// day and spawns real Minecraft particles around the player for ambience. Both
// are client-side only:
//
//   * Time  — applied via ClientWorld.setTime(worldAge, timeOfDay, tickDayTime)
//     each frame with tickDayTime=false, which freezes the client's own time
//     advance so our value persists to the sky/sun/moon. The server's real time
//     is untouched: mobs, sleeping and farms follow it, not us. A slider sets
//     the time and a speed makes the day advance locally.
//   * Particles — ClientWorld.addParticle with a ParticleTypes singleton, forced
//     to spawn regardless of the video Particles setting. Client-only.
//
// Nothing here renders to the ImGui overlay: unlike ESP/BlockESP this module
// has no draw pass. It only writes JVM state on the game thread each tick.

#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace vanta::modules::worldtweaks {

// Ambient particle style spawned continuously around the player. Off = none.
enum class AmbientMode : int {
    Off = 0, Fireflies, Embers, Snow, Sparkles, Magic, Souls
};

struct settings {
    // ── Time ── (slider + speed; no presets)
    bool  time_enabled = false;  // master switch for the time override
    int   time_ticks   = 6000;   // 0..24000 time-of-day the slider sets
    float time_speed   = 0.f;    // ticks advanced per real second; 0 = frozen at the slider

    // ── Particles (custom, drawn by us on the overlay, anchored in the world) ──
    int   ambient_mode    = static_cast<int>(AmbientMode::Off);
    int   ambient_density = 14;  // particles spawned per second (approx)
    int   ambient_radius  = 12;  // spawn box half-extent around the player, blocks
    float col_r = 1.0f, col_g = 0.82f, col_b = 0.30f; // base colour (warm gold)
    bool  rainbow = false;       // cycle hue over time/per-particle instead of colour
    float size    = 1.0f;        // particle size multiplier

    // ── Debug ──
    bool  debug = false;         // write %USERPROFILE%\Desktop\gl_world_trace.txt
};

extern settings          g_settings;
// Toggled from the module thread (onEnable/onDisable), read every frame from the
// game thread. Atomic so a toggle can't be lost to compiler CSE / tearing.
extern std::atomic<bool> g_enabled;

// Called on every GameLoopEvent from WorldTweaksModule. Applies the time
// override and simulates the particle pool (spawn + integrate). dt_ms is the
// frame delta so time and particles are frame-rate independent. Safe when no
// world is loaded (no-op).
void tick(void* env_opaque, float dt_ms);

// Called from the render pass (gl_hook, same slot as esp/blockesp render_text).
// Projects each live particle to the screen through the shared hitfx camera
// capture and draws it as a soft glow on the ImGui foreground draw list.
void render_text(int display_w, int display_h);

// Called from a PacketReceiveEvent listener on the netty thread. Returns true
// when the module + time override are on AND the packet is a
// WorldTimeUpdateS2CPacket — the caller cancels it so the server's periodic
// time sync never overwrites our value for a frame (the "sky flashes back to
// the real time for a millisecond" bug). packet is a jobject (void* to keep
// this header JNI-free). Safe on any thread; resolves the class lazily.
bool wants_cancel_time_packet(void* env_opaque, void* packet_opaque);

// Register JNI resolution seams. Idempotent. Called from the module's onInit.
void install();
void shutdown();

// Called from onDisable — lets the engine forget its interpolation state so the
// next enable eases from the world's real time instead of a stale value.
void on_disabled();

} // namespace vanta::modules::worldtweaks

