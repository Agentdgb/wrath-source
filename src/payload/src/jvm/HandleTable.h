#pragma once
#include <cstdint>
#include <jni.h>

namespace vanta::jvm {

// Thread-local pool that recycles jobject <-> uint64 handles for bridge natives.
// The pool is drained on PostTickEvent so we never NewGlobalRef in the hot path.
class HandleTable {
public:
    HandleTable();
    ~HandleTable();

    std::uint64_t acquire(JNIEnv* env, jobject o);
    jobject       resolve(std::uint64_t h) const noexcept;
    void          releaseAll(JNIEnv* env);
    void          tickReset(JNIEnv* env);   // drop handles that expired this tick

    static constexpr std::uint64_t kInvalid = 0;
private:
    struct Impl;
    Impl* impl_;
};

HandleTable& threadHandles();

} // namespace vanta::jvm
