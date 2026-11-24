#ifndef _JNIHOOK_H_
#define _JNIHOOK_H_

#include <jni.h>
#include <jvmti.h>

#define JNIHOOK_API
#define JNIHOOK_CALL

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	JNIHOOK_OK = 0,

	JNIHOOK_ERR_GET_JNI,
	JNIHOOK_ERR_GET_JVMTI,
	JNIHOOK_ERR_ADD_JVMTI_CAPS,
	JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK,

	JNIHOOK_ERR_JNI_OPERATION,
	JNIHOOK_ERR_JVMTI_OPERATION,
	JNIHOOK_ERR_CLASS_FILE_CACHE,
	JNIHOOK_ERR_JAVA_EXCEPTION,
} jnihook_result_t;

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Init(JavaVM *jvm);

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_InitWithEnv(JavaVM *jvm, jvmtiEnv *jvmti);

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Attach(jmethodID method, void *native_hook_method, jmethodID *original_method);

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Detach(jmethodID method);

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Shutdown();

#ifdef __cplusplus
}

#include <atomic>
// Publish the original before the native entry point becomes callable.
JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_AttachAtomic(jmethodID method, void *native_hook_method,
                     std::atomic<jmethodID> *original_method);
#endif
#endif
