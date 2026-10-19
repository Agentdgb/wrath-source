#pragma once
#include "jvm/JniRef.h"
#include "jvm/JvmContext.h"
#include "mappings/MappingRegistry.h"

#include <jni.h>
#include <string_view>

namespace vanta::jvm {

// Cached class/method/field lookups keyed by canonical name.
// Every module and hook site talks to the JVM through this API — never
// FindClass/GetMethodID directly.

jclass    findClassCanon(JNIEnv* env, std::string_view canon);

// Resolve a RUNTIME binary name (e.g. "net/minecraft/class_310") whichever
// loader holds it. Needed before the mapping registry is bound — version
// detection runs first and has only raw obf candidates to go on.
jclass    findClassAnyLoader(JNIEnv* env, const char* binaryName);
jmethodID methodCanon(JNIEnv* env, std::string_view ownerCanon, std::string_view methodCanon, bool isStatic = false);
jfieldID  fieldCanon(JNIEnv* env, std::string_view ownerCanon, std::string_view fieldCanon,  bool isStatic = false);

// Clear the caches (called on WorldChangeEvent when the class loader may have shifted).
void invalidateCaches();

// Clear pending exception if any and log it. Returns whether an exception was present.
bool checkAndClearException(JNIEnv* env, std::string_view scope);

// Small helpers on top of the JNI API to reduce per-callsite noise.
LocalObject callObjectCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method);
jint        callIntCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method);
jfloat      callFloatCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method);
jboolean    callBoolCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method);
void        callVoidCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method);

LocalObject staticCallObjectCanon(JNIEnv* env, std::string_view owner, std::string_view method);

jobject  getObjectFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field);
jfloat   getFloatFieldCanon(JNIEnv* env,  jobject self, std::string_view owner, std::string_view field);
jdouble  getDoubleFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field);
jint     getIntFieldCanon(JNIEnv* env,    jobject self, std::string_view owner, std::string_view field);
jboolean getBoolFieldCanon(JNIEnv* env,   jobject self, std::string_view owner, std::string_view field);

void setFloatFieldCanon(JNIEnv* env,  jobject self, std::string_view owner, std::string_view field, jfloat v);
void setDoubleFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field, jdouble v);
void setIntFieldCanon(JNIEnv* env,    jobject self, std::string_view owner, std::string_view field, jint v);
void setBoolFieldCanon(JNIEnv* env,   jobject self, std::string_view owner, std::string_view field, jboolean v);

} // namespace vanta::jvm
