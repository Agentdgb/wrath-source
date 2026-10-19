#pragma once
#include <jni.h>
#include <jvmti.h>

namespace vanta::entry {

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

// Full teardown + self-unload. Bound to F8 (or SHIFT+F8 to also purge disk config).
// Restores hooks and frees the DLL only when teardown is acknowledged and
// callbacks have drained. On failure the image remains resident.
void requestFullUnload();
void requestFullUnload(bool purgeConfig);

} // namespace vanta::entry
