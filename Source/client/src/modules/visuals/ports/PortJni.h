#pragma once
#include "jvm/JniHelper.h"
#include <stdexcept>
namespace vanta::modules::visualports::jni {
inline void check(JNIEnv *e) {
    if (!e->ExceptionCheck()) return;
    auto exception = e->ExceptionOccurred();
    e->ExceptionClear();
    std::string message = "Minecraft render call failed";
    auto type = e->GetObjectClass(exception);
    auto toString = type ? e->GetMethodID(type, "toString", "()Ljava/lang/String;") : nullptr;
    if (toString) {
        auto value = static_cast<jstring>(e->CallObjectMethod(exception, toString));
        if (value && !e->ExceptionCheck()) {
            if (auto text = e->GetStringUTFChars(value, nullptr)) {
                message = text;
                e->ReleaseStringUTFChars(value, text);
            }
            e->DeleteLocalRef(value);
        }
    }
    if (e->ExceptionCheck()) e->ExceptionClear();
    if (type) e->DeleteLocalRef(type);
    e->Throw(exception);
    e->DeleteLocalRef(exception);
    throw std::runtime_error(message);
}
inline jclass cls(JNIEnv *e, const char *n) {
    auto c = jvm::findClassAnyLoader(e, n);
    check(e);
    if (!c)
        throw std::runtime_error(n);
    return c;
}
inline jmethodID method(JNIEnv *e, jclass c, const char *n, const char *d) {
    auto m = e->GetMethodID(c, n, d);
    check(e);
    if (!m)
        throw std::runtime_error(n);
    return m;
}
inline jmethodID smethod(JNIEnv *e, jclass c, const char *n, const char *d) {
    auto m = e->GetStaticMethodID(c, n, d);
    check(e);
    if (!m)
        throw std::runtime_error(n);
    return m;
}
inline jfieldID field(JNIEnv *e, jclass c, const char *n, const char *d) {
    auto f = e->GetFieldID(c, n, d);
    check(e);
    if (!f)
        throw std::runtime_error(n);
    return f;
}
inline jmethodID optional(JNIEnv *e, jclass c, const char *n, const char *d) {
    auto m = e->GetMethodID(c, n, d);
    if (e->ExceptionCheck())
        e->ExceptionClear();
    return m;
}
inline jobject mc(JNIEnv *e) {
    auto c = cls(e, "net/minecraft/class_310");
    return e->CallStaticObjectMethod(c, smethod(e, c, "method_1551", "()Lnet/minecraft/class_310;"));
}
inline jobject player(JNIEnv *e, jobject mc) {
    return e->GetObjectField(mc,
                             field(e, cls(e, "net/minecraft/class_310"), "field_1724", "Lnet/minecraft/class_746;"));
}
inline jobject world(JNIEnv *e, jobject mc) {
    return e->GetObjectField(mc,
                             field(e, cls(e, "net/minecraft/class_310"), "field_1687", "Lnet/minecraft/class_638;"));
}
inline void translate(JNIEnv *e, jobject m, float x, float y, float z) {
    e->CallVoidMethod(m, method(e, cls(e, "net/minecraft/class_4587"), "method_46416", "(FFF)V"), x, y, z);
    check(e);
}
inline void rotate(JNIEnv *e, jobject m, float x, float y, float z) {
    auto q = cls(e, "org/joml/Quaternionf");
    auto obj = e->NewObject(q, method(e, q, "<init>", "()V"));
    e->CallObjectMethod(obj, method(e, q, "rotationZYX", "(FFF)Lorg/joml/Quaternionf;"), z, y, x);
    e->CallVoidMethod(m, method(e, cls(e, "net/minecraft/class_4587"), "method_22907", "(Lorg/joml/Quaternionf;)V"),
                      obj);
    e->DeleteLocalRef(obj);
    check(e);
}
} // namespace vanta::modules::visualports::jni
