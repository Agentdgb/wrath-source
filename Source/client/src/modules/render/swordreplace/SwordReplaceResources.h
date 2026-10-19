#pragma once
#include <jni.h>
namespace vanta::modules::swordreplace {
const char* prepareResources(JNIEnv* env, bool enabled, int model);
bool restoreResources(JNIEnv* env);
void shutdownResources(JNIEnv* env);
}
