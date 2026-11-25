#pragma once
#include <jni.h>

namespace dt::hook::bridge {

// Thread-local scratch slot used by Recipe B to smuggle a replaced return
// value from a bridge native back into Java without allocating.
// Recipe B populates ALT with a boxed primitive (long[] with a single element)
// before its INVOKESTATIC returns nonzero.
class AltSlot {
public:
    static jobject threadLocalArray(JNIEnv* env);   // ThreadLocal<long[]>
    static void    setLong(JNIEnv* env, jlong v);
    static jlong   getLong(JNIEnv* env);
};

} // namespace dt::hook::bridge
