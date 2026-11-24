// Include the implementation in this isolated executable to inject lifecycle
// failures without exposing test controls in the production DLL/API.
#include "../vendor/jnihook/src/jnihook.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
JNIEnv testEnv{};
JavaVM testVm{};
jvmtiEnv testTi{};
int restores = 0, deletedRefs = 0, disposals = 0;
bool failRestore = false, failDisable = false, failDispose = false;
const auto failedClass = reinterpret_cast<jclass>(std::uintptr_t{1});

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
jint JNICALL getEnv(JavaVM*, void** result, jint) { *result = &testEnv; return JNI_OK; }
void JNICALL deleteRef(JNIEnv*, jobject) { ++deletedRefs; }
jvmtiError JNICALL redefine(jvmtiEnv*, jint, const jvmtiClassDefinition* definition) {
    ++restores;
    return failRestore && definition->klass == failedClass ? JVMTI_ERROR_INTERNAL : JVMTI_ERROR_NONE;
}
jvmtiError JNICALL setMode(jvmtiEnv*, jvmtiEventMode, jvmtiEvent, jthread, ...) {
    return failDisable ? JVMTI_ERROR_INTERNAL : JVMTI_ERROR_NONE;
}
jvmtiError JNICALL setCallbacks(jvmtiEnv*, const jvmtiEventCallbacks*, jint) { return JVMTI_ERROR_NONE; }
jvmtiError JNICALL dispose(jvmtiEnv*) {
    ++disposals;
    return failDispose ? JVMTI_ERROR_INTERNAL : JVMTI_ERROR_NONE;
}
void seed() {
    std::ifstream input(WRATH_JNIHOOK_FIXTURE, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    require(!bytes.empty(), "missing class fixture");
    g_jnihook = std::make_unique<jnihook_t>(jnihook_t{&testVm, &testTi});
    for (unsigned i = 1; i <= 2; ++i) {
        const auto key = std::to_string(i);
        g_class_file_cache[key] = ClassFile::load(bytes.data());
        require(g_class_file_cache[key] != nullptr, "invalid class fixture");
        g_hooks[key].push_back({{"method_52906", "(Lnet/minecraft/class_2596;Lio/netty/channel/ChannelFutureListener;Z)V", 0}, nullptr});
        g_declaring_classes[key] = reinterpret_cast<jclass>(std::uintptr_t{i});
        g_original_classes[key] = reinterpret_cast<jclass>(std::uintptr_t{i + 10});
    }
    restores = deletedRefs = disposals = 0;
}
}

int main() {
    JNINativeInterface_ jni{}; jni.DeleteGlobalRef = deleteRef; testEnv.functions = &jni;
    JNIInvokeInterface_ vm{}; vm.GetEnv = getEnv; testVm.functions = &vm;
    jvmtiInterface_1_ ti{};
    ti.RedefineClasses = redefine; ti.SetEventNotificationMode = setMode;
    ti.SetEventCallbacks = setCallbacks; ti.DisposeEnvironment = dispose; testTi.functions = &ti;
    try {
        seed(); failRestore = true;
        require(JNIHook_Shutdown() == JNIHOOK_ERR_JVMTI_OPERATION, "restoration error was lost");
        require(restores == 2, "shutdown abandoned other classes after the first failure");
        require(g_hooks["1"].size() == 1 && g_hooks["2"].empty(), "failed hook state was not preserved");
        require(g_class_file_cache.size() == 2 && deletedRefs == 0 && disposals == 0,
            "partial shutdown released live state");
        failRestore = false;
        require(JNIHook_Shutdown() == JNIHOOK_OK && deletedRefs == 4 && !g_jnihook,
            "partial shutdown could not be retried");

        seed(); vanta::g_nativeCallbacks.store(1);
        require(JNIHook_Shutdown() != JNIHOOK_OK && deletedRefs == 0 && g_class_file_cache.size() == 2,
            "active callback lost its class cache or original references");
        vanta::g_nativeCallbacks.store(0);
        require(JNIHook_Shutdown() == JNIHOOK_OK, "drain failure was not retryable");

        seed(); failDisable = true;
        require(JNIHook_Shutdown() != JNIHOOK_OK && deletedRefs == 0 && disposals == 0,
            "event shutdown failure was ignored");
        failDisable = false; failDispose = true;
        require(JNIHook_Shutdown() != JNIHOOK_OK && deletedRefs == 0 && g_jnihook,
            "environment disposal failure discarded retry state");
        failDispose = false;
        require(JNIHook_Shutdown() == JNIHOOK_OK && deletedRefs == 4,
            "environment shutdown retry failed");
        std::cout << "Best-effort restore, active callback retention and lifecycle failure retries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
