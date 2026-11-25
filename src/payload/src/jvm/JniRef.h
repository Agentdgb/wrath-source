#pragma once
#include <jni.h>
#include <string>
#include <utility>

namespace dt::jvm {

// RAII local reference — freed automatically when the scope exits. Move-only.
template <class J>
class LocalRef {
    JNIEnv* env_ = nullptr;
    J       obj_ = nullptr;
public:
    LocalRef() = default;
    LocalRef(JNIEnv* env, J obj) noexcept : env_(env), obj_(obj) {}
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    LocalRef(LocalRef&& o) noexcept : env_(o.env_), obj_(o.obj_) { o.obj_ = nullptr; }
    LocalRef& operator=(LocalRef&& o) noexcept {
        if (this != &o) { reset(); env_ = o.env_; obj_ = o.obj_; o.obj_ = nullptr; }
        return *this;
    }
    ~LocalRef() { reset(); }

    void reset() noexcept {
        if (env_ && obj_) env_->DeleteLocalRef(obj_);
        obj_ = nullptr;
    }
    J    get()      const noexcept { return obj_; }
    J    release()  noexcept       { J o = obj_; obj_ = nullptr; return o; }
    JNIEnv* env()   const noexcept { return env_; }
    explicit operator bool() const noexcept { return obj_ != nullptr; }
};

// RAII global reference — must be freed on the thread that has an env.
template <class J>
class GlobalRef {
    JNIEnv* env_ = nullptr;
    J       obj_ = nullptr;
public:
    GlobalRef() = default;
    GlobalRef(JNIEnv* env, J local) noexcept
        : env_(env), obj_(env && local ? static_cast<J>(env->NewGlobalRef(local)) : nullptr) {}
    GlobalRef(const GlobalRef&) = delete;
    GlobalRef& operator=(const GlobalRef&) = delete;
    GlobalRef(GlobalRef&& o) noexcept : env_(o.env_), obj_(o.obj_) { o.obj_ = nullptr; }
    GlobalRef& operator=(GlobalRef&& o) noexcept {
        if (this != &o) { reset(); env_ = o.env_; obj_ = o.obj_; o.obj_ = nullptr; }
        return *this;
    }
    ~GlobalRef() { reset(); }

    void reset() noexcept {
        if (env_ && obj_) env_->DeleteGlobalRef(obj_);
        obj_ = nullptr;
    }
    J   get() const noexcept { return obj_; }
    explicit operator bool() const noexcept { return obj_ != nullptr; }
};

using LocalObject  = LocalRef<jobject>;
using LocalClass   = LocalRef<jclass>;
using LocalString  = LocalRef<jstring>;
using GlobalObject = GlobalRef<jobject>;
using GlobalClass  = GlobalRef<jclass>;

// Convert jstring -> std::string via GetStringUTFChars. Safe on null.
std::string toStdString(JNIEnv* env, jstring js);

} // namespace dt::jvm
