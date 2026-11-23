#pragma once
#include "hook/HookId.h"

#include <cstdint>
#include <jni.h>
#include <jvmti.h>

namespace vanta::hook {

class HookManager {
public:
    static HookManager& get();

    // Registers the ClassFileLoadHook callback, defines HookBridge, registers
    // its natives. Called from Bootstrap after MappingRegistry is ready.
    void install(jvmtiEnv* jvmti, JNIEnv* env);

    // Iterate all loaded classes, filter to canonical targets, and issue
    // RetransformClasses in bounded batches. Safe to call any number of times.
    void retransformKnown();

    void armHook(HookId h);       // lazy hooks: mark active + retransform
    void disarmHook(HookId h);    // opposite

    // Called from ClassFileLoadHook thread (any) with class bytes to (maybe)
    // rewrite. Populates newBytes when a transform applies; returns whether one did.
    bool transformIncoming(std::string_view runtimeClassName,
                           const std::uint8_t* classBytes,
                           std::size_t         classLen,
                           std::uint8_t**      newBytes,
                           std::size_t*        newLen);

    // Restore originals for hot-unload.
    void uninstall();

    std::size_t transformedClassCount() const noexcept;
private:
    struct Impl; Impl* impl_ = nullptr;

    HookManager();
    ~HookManager();
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;
};

} // namespace vanta::hook
