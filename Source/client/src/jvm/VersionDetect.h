#pragma once
#include "jvm/LoaderProbe.h"

#include <jni.h>
#include <string>

namespace vanta::jvm {

struct RuntimeInfo {
    std::string mcVersion;   // e.g. "1.21.4"
    Loader      loader = Loader::Unknown;
    bool        isDev  = false;
};

RuntimeInfo        detectRuntime(JNIEnv* env);
const RuntimeInfo& currentRuntime();

} // namespace vanta::jvm
