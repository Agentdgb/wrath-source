#pragma once
#include <jni.h>
#include <string_view>

namespace vanta::hook::bridge {

// Camouflaged JDK internal class name that replaces "dev/vanta/HookBridge".
// Excluded from Ocean.ac inspection (gOg prefix rule: "jdk.internal") and
// blends seamlessly with OpenJDK/HotSpot runtime math utilities.
inline constexpr std::string_view kBridgeClassInternalName = "jdk/internal/math/DoublePrecision";
inline constexpr std::string_view kBridgeClassPackageDot   = "jdk.internal.math.DoublePrecision";

// Defines the camouflaged bridge class into the bootstrap class loader.
// Returns a global jclass reference (or nullptr on failure).
jclass defineHookBridgeClass(JNIEnv* env);

} // namespace vanta::hook::bridge
