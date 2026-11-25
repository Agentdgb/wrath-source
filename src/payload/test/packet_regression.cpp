#include "hook/PacketEvents.h"
#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include <jnihook.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <latch>
#include <thread>
#include <atomic>

namespace dt::jvm {
JvmContext testContext;
JvmContext& ctx() noexcept { return testContext; }
JNIEnv* thisThreadEnv() {
    JNIEnv* env = nullptr;
    if (auto vm = ctx().vm.load()) vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    return env;
}
JniScope::JniScope() { env_ = thisThreadEnv(); }
JniScope::~JniScope() = default;
}

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() {
    std::filesystem::path jvmPath(WRATH_TEST_JVM_DLL);
    jvmPath.make_preferred();
    auto binDir = jvmPath.parent_path().parent_path();
    const wchar_t* curPath = _wgetenv(L"PATH");
    std::wstring newPath = binDir.wstring() + (curPath ? (L";" + std::wstring(curPath)) : L"");
    SetEnvironmentVariableW(L"PATH", newPath.c_str());
    SetDllDirectoryW(binDir.c_str());

    using SetDefaultDllDirsFn = BOOL(WINAPI*)(DWORD);
    using AddDllDirFn = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
    auto hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        auto pSetDefault = reinterpret_cast<SetDefaultDllDirsFn>(GetProcAddress(hK32, "SetDefaultDllDirectories"));
        auto pAddDir = reinterpret_cast<AddDllDirFn>(GetProcAddress(hK32, "AddDllDirectory"));
        if (pSetDefault && pAddDir) {
            pSetDefault(0x00001000 | 0x00000800);
            pAddDir(binDir.c_str());
        }
    }

    LoadLibraryW((binDir / L"vcruntime140.dll").c_str());
    LoadLibraryW((binDir / L"vcruntime140_1.dll").c_str());
    LoadLibraryW((binDir / L"msvcp140.dll").c_str());
    LoadLibraryW((binDir / L"jli.dll").c_str());

    HMODULE runtime = LoadLibraryW(jvmPath.c_str());
    if (!runtime) runtime = LoadLibraryExW(jvmPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!runtime) {
        std::cerr << "Could not load the test JVM, error: " << GetLastError() << "\n";
        return 1;
    }
    using CreateVm = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<CreateVm>(GetProcAddress(runtime, "JNI_CreateJavaVM"));
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    std::string classpath = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
    JavaVMOption options[] = {{classpath.data(), nullptr}, {const_cast<char*>("-Xverify:all"), nullptr}};
    JavaVMInitArgs arguments{JNI_VERSION_1_8, 2, options, JNI_FALSE};
    if (!create || create(&vm, reinterpret_cast<void**>(&env), &arguments) != JNI_OK) return 1;
    dt::jvm::ctx().vm.store(vm);
    int result = 0;
    try {
        auto cls = env->FindClass("net/minecraft/class_2535");
        auto packetClass = env->FindClass("net/minecraft/class_2596");
        require(cls && packetClass && !env->ExceptionCheck(), "fixture classes not loaded");
        auto cachedClass = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2535");
        auto cachedAgain = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2535");
        require(cachedClass && cachedAgain && env->IsSameObject(cachedClass, cachedAgain)
            && env->GetObjectRefType(cachedAgain) == JNILocalRefType, "runtime cache did not return owned local references");
        env->DeleteLocalRef(cachedClass); env->DeleteLocalRef(cachedAgain);
        require(!dt::jvm::findClassAnyLoader(env, "net/minecraft/missing_test_class")
            && !dt::jvm::findClassAnyLoader(env, "net/minecraft/missing_test_class")
            && !env->ExceptionCheck(), "missing lookup leaked an exception or failed retry caching");
        auto object = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
        auto packet = env->NewObject(packetClass, env->GetMethodID(packetClass, "<init>", "()V"));
        auto send = env->GetMethodID(cls, "method_52906", "(Lnet/minecraft/class_2596;Lio/netty/channel/ChannelFutureListener;Z)V");
        auto receive = env->GetMethodID(cls, "method_10770", "(Lio/netty/channel/ChannelHandlerContext;Lnet/minecraft/class_2596;)V");
        auto sends = env->GetFieldID(cls, "sends", "I");
        auto receives = env->GetFieldID(cls, "receives", "I");
        auto flushed = env->GetFieldID(cls, "flushed", "Z");
        auto fail = env->GetFieldID(packetClass, "fail", "Z");
        // Keep Java traffic active while Attach publishes the originals and
        // enables each native entry. No call may disappear during that window.
        auto installObject = env->NewGlobalRef(env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V")));
        auto installPacket = env->NewGlobalRef(packet);
        std::latch trafficStarted(1);
        std::atomic<bool> installationDone{false}, trafficFailed{false};
        unsigned installCalls = 0;
        std::jthread traffic([&] {
            JNIEnv* trafficEnv = nullptr;
            if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&trafficEnv), nullptr) != JNI_OK) {
                trafficFailed.store(true); trafficStarted.count_down(); return;
            }
            trafficStarted.count_down();
            do {
                trafficEnv->CallVoidMethod(installObject, send, installPacket, nullptr, JNI_FALSE);
                if (trafficEnv->ExceptionCheck()) {
                    trafficEnv->ExceptionClear(); trafficFailed.store(true); break;
                }
                ++installCalls;
            } while (!installationDone.load(std::memory_order_acquire) && installCalls < 100000);
            vm->DetachCurrentThread();
        });
        trafficStarted.wait();
        dt::hook::packets::install();
        installationDone.store(true, std::memory_order_release);
        traffic.join();
        const bool allForwarded = !trafficFailed.load() && installCalls > 0
            && env->GetIntField(installObject, sends) == static_cast<jint>(installCalls);
        env->DeleteGlobalRef(installObject); env->DeleteGlobalRef(installPacket);
        require(dt::hook::packets::ready(), "packet event installation failed");
        require(allForwarded, "installation dropped a concurrent vanilla packet call");
        {
            int queuedEvents = 0;
            auto queuedListener = dt::EventBus::get().subscribe<dt::events::PacketSendEvent>(
                [&](auto&) { ++queuedEvents; });
            auto queued = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
            auto connected = env->GetFieldID(cls, "connected", "Z");
            auto drain = env->GetMethodID(cls, "method_10751", "()V");
            for (int cycle = 0; cycle < 100; ++cycle) {
                env->SetBooleanField(queued, connected, JNI_FALSE);
                env->CallVoidMethod(queued, send, packet, nullptr, JNI_TRUE);
                require(!env->ExceptionCheck(), "queueing a disconnected send raised an exception");
                require(queuedEvents == cycle, "queued send fired before the final channel write");
                env->SetBooleanField(queued, connected, JNI_TRUE);
                env->CallVoidMethod(queued, drain);
                require(!env->ExceptionCheck(), "queued send rejected the real connection (disconnect ClassCastException)");
                require(env->GetIntField(queued, sends) == cycle + 1 && queuedEvents == cycle + 1,
                    "queued packet was lost or dispatched more than once");
                require(env->GetBooleanField(queued, flushed), "queued packet lost its flush argument");
            }
            env->DeleteLocalRef(queued);
        }
        int sentEvents = 0, receivedEvents = 0;
        bool cancel = false;
        auto outgoing = dt::EventBus::get().subscribe<dt::events::PacketSendEvent>([&](auto& event) {
            ++sentEvents; if (cancel) event.cancel();
        });
        auto incoming = dt::EventBus::get().subscribe<dt::events::PacketReceiveEvent>([&](auto& event) {
            ++receivedEvents; if (cancel) event.cancel();
        });
        env->CallVoidMethod(object, send, packet, nullptr, JNI_TRUE);
        env->CallVoidMethod(object, receive, nullptr, packet);
        require(!env->ExceptionCheck() && sentEvents == 1 && receivedEvents == 1, "packet events not delivered once");
        require(env->GetIntField(object, sends) == 1 && env->GetIntField(object, receives) == 1
            && env->GetBooleanField(object, flushed), "original networking arguments/behavior changed");
        cancel = true;
        env->CallVoidMethod(object, send, packet, nullptr, JNI_FALSE);
        env->CallVoidMethod(object, receive, nullptr, packet);
        require(env->GetIntField(object, sends) == 1 && env->GetIntField(object, receives) == 1, "cancelled packet reached original");
        cancel = false;
        env->SetBooleanField(packet, fail, JNI_TRUE);
        env->CallVoidMethod(object, send, packet, nullptr, JNI_FALSE);
        require(env->ExceptionCheck(), "original Java exception was swallowed");
        env->ExceptionClear();
        env->SetBooleanField(packet, fail, JNI_FALSE);
        std::latch entered(1), release(1);
        auto blocker = dt::EventBus::get().subscribe<dt::events::PacketSendEvent>([&] {
            entered.count_down(); release.wait();
        });
        auto retainedObject = env->NewGlobalRef(object);
        auto retainedPacket = env->NewGlobalRef(packet);
        std::jthread activeCallback([&] {
            JNIEnv* workerEnv = nullptr;
            vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&workerEnv), nullptr);
            workerEnv->CallVoidMethod(retainedObject, send, retainedPacket, nullptr, JNI_FALSE);
            vm->DetachCurrentThread();
        });
        entered.wait();
        // Packet shutdown must drain BEFORE detaching: the blocked listener's
        // original method must remain usable after the timeout and on retry.
        const bool blockedShutdown = dt::hook::packets::shutdown();
        const bool stopped = !dt::hook::packets::ready();
        // New traffic bypasses listeners while shutdown waits, but still forwards.
        env->CallVoidMethod(object, send, packet, nullptr, JNI_FALSE);
        const bool forwardedWhileStopping = !env->ExceptionCheck()
            && env->GetIntField(object, sends) == 2 && sentEvents == 4;
        release.count_down();
        activeCallback.join();
        require(!blockedShutdown && stopped, "packet shutdown did not refuse an active callback");
        require(forwardedWhileStopping, "stopping dropped vanilla traffic or dispatched listeners");
        blocker.reset();
        env->DeleteGlobalRef(retainedObject); env->DeleteGlobalRef(retainedPacket);
        require(dt::hook::packets::shutdown(), "packet event detach failed");
        require(dt::hook::packets::shutdown(), "packet shutdown was not idempotent");
        require(JNIHook_Shutdown() == JNIHOOK_OK, "JNIHook shutdown failed");
        env->CallVoidMethod(object, send, packet, nullptr, JNI_FALSE);
        require(env->GetIntField(object, sends) == 4 && sentEvents == 4, "vanilla behavior not restored after detach");
        std::cout << "Real JVM packet events, 100 queued reconnect/drain cycles, cancellation, original arguments, "
                     "exceptions, active-callback refusal and detach passed under -Xverify:all\n";
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        std::cerr << error.what() << '\n'; result = 1;
    }
    dt::jvm::invalidateCaches();
    vm->DestroyJavaVM();
    return result;
}
