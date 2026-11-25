#pragma once
#include <jni.h>
#include <jvmti.h>
#include <atomic>

namespace dt::jvm {

struct JvmContext {
    std::atomic<JavaVM*> vm{nullptr};
    std::atomic<jvmtiEnv*> jvmti{nullptr};
};

JvmContext& ctx() noexcept;

void init(JavaVM* vm);                        // called from Agent_On*
void initFromRunningProcess();                // called from DtInit (LoadLibrary path)
void requestCapabilities();                   // requests full hard-required set + optional
void shutdown();

JNIEnv* thisThreadEnv();                      // attaches on first use, AttachCurrentThreadAsDaemon
void    detachThisThreadIfAttached();         // called by thread_local dtor via JniScope

// RAII: acquire an env for the current scope. On threads not yet attached, attach
// as daemon; detach happens when the *thread* exits (never mid-thread scope).
class JniScope {
    JNIEnv* env_    = nullptr;
    bool    isNoop_ = false;
public:
    JniScope();
    ~JniScope();
    JniScope(const JniScope&) = delete;
    JniScope& operator=(const JniScope&) = delete;

    JNIEnv* get()        const noexcept { return env_; }
    JNIEnv* operator->() const noexcept { return env_; }
    explicit operator bool() const noexcept { return env_ != nullptr; }
};

} // namespace dt::jvm
