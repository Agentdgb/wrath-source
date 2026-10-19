#pragma once
#include <jni.h>

namespace vanta::hook::bridge {

// Called once at Bootstrap after HookBridge is defined. Registers the entire
// native trampoline table so subsequent INVOKESTATIC lands directly in C++
// without JNI resolver overhead per call.
void registerNatives(JNIEnv* env, jclass bridgeClass);

// Return codes bridge natives use consistently:
//   0 -> continue original behavior (do not cancel)
//   1 -> cancel/short-circuit (Recipe A returns from method here)
inline constexpr jint kBridgeContinue = 0;
inline constexpr jint kBridgeCancel   = 1;

} // namespace vanta::hook::bridge
