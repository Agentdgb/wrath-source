#pragma once
#include <jni.h>

namespace dt::jvm {

enum class Loader { Vanilla, Fabric, NeoForge, Forge, Unknown };

Loader detectLoader(JNIEnv* env);
const char* loaderName(Loader l) noexcept;

} // namespace dt::jvm
