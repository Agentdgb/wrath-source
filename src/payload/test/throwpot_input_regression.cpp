#include "modules/combat/ThrowPotInput.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    using namespace dt::modules::throwpot;
    const std::filesystem::path runtimePath(WRATH_TEST_JVM_DLL);
    const auto bin = runtimePath.parent_path().parent_path();
    SetDllDirectoryW(bin.c_str());
    HMODULE runtime = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!runtime) {
        std::cerr << "JVM load failed: " << GetLastError() << '\n';
        return 1;
    }
    using Create = jint(JNICALL *)(JavaVM **, void **, void *);
    auto create = reinterpret_cast<Create>(GetProcAddress(runtime, "JNI_CreateJavaVM"));
    std::string classpath = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
    JavaVMOption option{classpath.data(), nullptr};
    JavaVMInitArgs args{JNI_VERSION_1_8, 1, &option, JNI_FALSE};
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;
    if (!create || create(&vm, reinterpret_cast<void **>(&env), &args) != JNI_OK)
        return 1;
    int result = 0;
    try {
        auto cls = env->FindClass("InputFixture");
        auto keyClass = env->FindClass("InputFixture$Key");
        require(cls && keyClass, "fixture missing");
        auto fixture = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
        auto use = env->GetObjectField(fixture, env->GetFieldID(cls, "use", "LInputFixture$Key;"));
        auto clicks = env->GetFieldID(keyClass, "clickCount", "I");
        auto selected = env->GetFieldID(cls, "selected", "I");
        auto count = env->GetFieldID(cls, "count", "I");
        auto packets = env->GetFieldID(cls, "packets", "Ljava/lang/String;");
        auto tick = env->GetMethodID(cls, "tick", "()V");
        auto trace = [&] {
            auto str = static_cast<jstring>(env->GetObjectField(fixture, packets));
            const char *bytes = env->GetStringUTFChars(str, nullptr);
            const std::string text(bytes);
            env->ReleaseStringUTFChars(str, bytes);
            env->DeleteLocalRef(str);
            return text;
        };
        env->SetIntField(fixture, selected, 3);
        require(queueUse(env, use, clicks), "press was not queued without a hook");
        require(!queueUse(env, use, clicks), "one press queued twice");
        require(trace().empty(), "render-side queue emitted packets");
        require(useProgress(2, 2, true, 1, 10) == UseProgress::Waiting, "did not wait for native input");
        env->CallVoidMethod(fixture, tick);
        require(trace() == "slot:3;use;move;", "slot/use/movement order changed");
        require(useProgress(2, env->GetIntField(fixture, count), true, env->GetIntField(use, clicks), 50) ==
                    UseProgress::Thrown,
                "native consumption not acknowledged");
        env->SetIntField(fixture, selected, 0);
        env->CallVoidMethod(fixture, tick);
        require(trace() == "slot:3;use;move;slot:0;move;", "restore emitted another use");
        require(queueUse(env, use, clicks), "completed press could not be repeated");
        env->SetIntField(use, clicks, 2); // one additional physical click
        cancelUse(env, use, clicks);
        require(env->GetIntField(use, clicks) == 1, "cancellation consumed a physical click");
        cancelUse(env, use, clicks);
        require(useProgress(1, 1, true, 0, 50) == UseProgress::Rejected, "rejected use stayed armed");
        require(useProgress(1, 1, true, 1, 500) == UseProgress::TimedOut, "missing tick stayed armed forever");
        require(useProgress(1, 1, false, 1, 10) == UseProgress::Cancelled, "manual slot was overwritten");
        require(!env->ExceptionCheck(), "JNI exception during input transport");
        std::cout << "Native use press, acknowledgement, restoration, repeat and "
                     "cancellation passed\n";
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        result = 1;
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    vm->DestroyJavaVM();
    return result;
}
