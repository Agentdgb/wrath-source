#pragma once
#include <jni.h>
#include <jvmti.h>

#ifdef _DEBUG
#define VANTA_AGENT_EXPORT JNIEXPORT
#else
#define VANTA_AGENT_EXPORT
#endif

extern "C" {

VANTA_AGENT_EXPORT jint JNICALL Agent_OnLoad   (JavaVM* vm, char* options, void* reserved);
VANTA_AGENT_EXPORT jint JNICALL Agent_OnAttach (JavaVM* vm, char* options, void* reserved);
VANTA_AGENT_EXPORT void JNICALL Agent_OnUnload (JavaVM* vm);

}  // extern C
