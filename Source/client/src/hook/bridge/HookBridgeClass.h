#pragma once
#include <jni.h>

namespace vanta::hook::bridge {

// Defines dev.vanta.HookBridge into the bootstrap class loader.
// Returns a global jclass reference (or nullptr on failure).
jclass defineHookBridgeClass(JNIEnv* env);

} // namespace vanta::hook::bridge
