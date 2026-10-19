#pragma once
#include <jni.h>

namespace vanta::jvm {

enum class Loader { Vanilla, Fabric, NeoForge, Forge, Unknown };

Loader detectLoader(JNIEnv* env);
const char* loaderName(Loader l) noexcept;

} // namespace vanta::jvm
