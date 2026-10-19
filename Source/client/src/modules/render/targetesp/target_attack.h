#pragma once
#include <jni.h>
namespace vanta::modules::targetesp::attack {
bool install(JNIEnv* env);
void set_enabled(bool enabled);
void reset(JNIEnv* env);
// Transfers the latest attacked entity into the caller's local JNI frame.
jobject consume(JNIEnv* env, double* age_seconds = nullptr);
void shutdown();
}
