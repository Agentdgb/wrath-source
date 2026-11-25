#pragma once
#include <jni.h>
#include <jvmti.h>
#include <atomic>

namespace dt::entry {

extern std::atomic<bool> g_agentPresent;

inline void setAgentPresent(bool present = true) noexcept {
    g_agentPresent.store(present, std::memory_order_release);
}

inline bool isAgentPresent() noexcept {
    return g_agentPresent.load(std::memory_order_acquire);
}

// NativeInject (not LoadLibrary) — Windows headers macro-expand LoadLibrary to
// LoadLibraryA/W depending on UNICODE, breaking the enum.
enum class Door { AgentOnLoad, AgentOnAttach, NativeInject };

// Idempotent bootstrap. Any entry point can call this; only one wins.
void bootstrap(JavaVM* vm, const char* options, Door door);
bool bootstrapFinished() noexcept;
enum class UnloadState { Idle, Stopping, StoppedResident, Failed, ReleaseRequested };
UnloadState unloadState() noexcept;
unsigned long unloadError() noexcept;

void shutdown();

// Full teardown + self-unload. Profiles and user data are always preserved.
// Restores hooks and frees the DLL only when teardown is acknowledged and
// callbacks have drained. On failure the image remains resident.
// Images absent from the Windows loader are kept resident; manual-map
// teardown is not implemented by this path.
void requestFullUnload();
// Legacy argument is ignored; it cannot request profile deletion.
void requestFullUnload(bool purgeConfig);

} // namespace dt::entry
