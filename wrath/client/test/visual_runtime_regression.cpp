#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "core/event/GameFramePump.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "modules/render/chams/item_shader_hook.h"
#include "modules/visuals/ports/VisualPorts.h"
#include <filesystem>
#include <iostream>
#include <jnihook.h>
#include <thread>
#include <windows.h>
namespace vanta::jvm {
JvmContext state;
JvmContext &ctx() noexcept { return state; }
JNIEnv *thisThreadEnv() {
    JNIEnv *e = nullptr;
    auto vm = state.vm.load();
    if (vm)
        vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_8);
    return e;
}
JniScope::JniScope() { env_ = thisThreadEnv(); }
JniScope::~JniScope() = default;
jclass findClassAnyLoader(JNIEnv *e, const char *n) { return e->FindClass(n); }
} // namespace vanta::jvm
namespace vanta::hook::packets {
void install() {}
} // namespace vanta::hook::packets
namespace vanta::modules::bloom {
bool interested = false;
bool needsWorld() { return interested; }
void shutdown() {}
} // namespace vanta::modules::bloom
namespace vanta::modules::itemshader {
bool wanted() { return false; }
void note_firstperson_render_call() {}
void set_wanted(bool) {}
void set_effect(bool) {}
} // namespace vanta::modules::itemshader
namespace vanta::modules::visualports {
SwingConfig swing;
int statuses = 0, lastCode = 0, worldFrames = 0;
void queueStatus(JNIEnv *, jobject, jbyte b) {
    ++statuses;
    lastCode = b;
}
void afterWorld(JNIEnv *) { ++worldFrames; }
} // namespace vanta::modules::visualports
using namespace vanta::modules::visualports;
void require(bool okay, const char *text) {
    if (!okay)
        throw std::runtime_error(text);
}
int main() {
    JavaVM *vm = nullptr;
    JNIEnv *e = nullptr;
    int result = 0;
    try {
        auto path = std::filesystem::path(WRATH_TEST_JVM_DLL);
        auto binDir = path.parent_path().parent_path();
        SetDllDirectoryW(binDir.c_str());
        LoadLibraryW((binDir / L"jli.dll").c_str());
        auto dll = LoadLibraryW(path.c_str());
        if (!dll) dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(dll != nullptr, "Load test JVM");
        using Create = jint(JNICALL *)(JavaVM **, void **, void *);
        auto create = reinterpret_cast<Create>(GetProcAddress(dll, "JNI_CreateJavaVM"));
        std::string cp = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
        JavaVMOption options[] = {{cp.data(), nullptr}, {const_cast<char *>("-Xverify:all"), nullptr}};
        JavaVMInitArgs args{JNI_VERSION_1_8, 2, options, JNI_FALSE};
        require(create && create(&vm, reinterpret_cast<void **>(&e), &args) == JNI_OK, "Create test JVM");
        vanta::jvm::ctx().vm.store(vm);
        auto mirror = vanta::EventBus::get().subscribe<vanta::events::GameLoopEvent>([](auto &) {
            swing.enabled = true;
            vanta::modules::bloom::interested = true;
        });
        vanta::core::pumpGameFrame();
        require(std::string(swingStatus()) == "Custom first-person swing animations",
                "Frame pump did not install swing independently");
        auto mcClass = e->FindClass("net/minecraft/class_310");
        auto mc = e->CallStaticObjectMethod(
            mcClass, e->GetStaticMethodID(mcClass, "method_1551", "()Lnet/minecraft/class_310;"));
        auto player = e->GetObjectField(mc, e->GetFieldID(mcClass, "field_1724", "Lnet/minecraft/class_746;"));
        auto rendererClass = e->FindClass("net/minecraft/class_759"),
             matrixClass = e->FindClass("net/minecraft/class_4587"),
             handClass = e->FindClass("net/minecraft/class_1268");
        auto renderer = e->NewObject(rendererClass, e->GetMethodID(rendererClass, "<init>", "()V"));
        auto matrices = e->NewObject(matrixClass, e->GetMethodID(matrixClass, "<init>", "()V"));
        auto main = e->GetStaticObjectField(handClass,
                                            e->GetStaticFieldID(handClass, "field_5808", "Lnet/minecraft/class_1268;"));
        auto off = e->GetStaticObjectField(handClass,
                                           e->GetStaticFieldID(handClass, "OFF_HAND", "Lnet/minecraft/class_1268;"));
        auto render = e->GetMethodID(rendererClass, "method_3228",
                                     "(Lnet/minecraft/class_742;FFLnet/minecraft/class_1268;FLnet/minecraft/"
                                     "class_1799;FLnet/minecraft/class_4587;Lnet/minecraft/class_11659;I)V");
        auto depth = e->GetFieldID(matrixClass, "depth", "I"),
             translations = e->GetFieldID(matrixClass, "translations", "I");
        auto value = e->GetFieldID(rendererClass, "lastSwing", "F"),
             seen = e->GetFieldID(rendererClass, "depthSeen", "I");
        for (int style = 0; style < 6; ++style) {
            swing.style = style;
            auto previous = e->GetIntField(matrices, translations);
            e->CallVoidMethod(renderer, render, player, 0.f, 0.f, main, .4f, nullptr, 0.f, matrices, nullptr, 0);
            require(!e->ExceptionCheck(), "Swing original threw");
            require(e->GetIntField(matrices, translations) > previous,
                    "Swing did not transform the actual render path");
            require(e->GetFloatField(renderer, value) == 0 && e->GetIntField(renderer, seen) == 1,
                    "Vanilla swing was not replaced inside matrix scope");
            require(e->GetIntField(matrices, depth) == 0, "Swing leaked a matrix");
        }
        swing.offhand = false;
        e->CallVoidMethod(renderer, render, player, 0.f, 0.f, off, .4f, nullptr, 0.f, matrices, nullptr, 0);
        require(e->GetFloatField(renderer, value) == .4f, "Offhand filter changed vanilla swing");
        auto fail = e->GetFieldID(rendererClass, "fail", "Z");
        e->SetBooleanField(renderer, fail, JNI_TRUE);
        e->CallVoidMethod(renderer, render, player, 0.f, 0.f, main, .4f, nullptr, 0.f, matrices, nullptr, 0);
        require(e->ExceptionCheck(), "Original render exception swallowed");
        e->ExceptionClear();
        require(e->GetIntField(matrices, depth) == 0, "Exception leaked matrix scope");
        e->SetBooleanField(renderer, fail, JNI_FALSE);
        auto packetClass = e->FindClass("net/minecraft/class_2663");
        auto packet = e->NewObject(packetClass, e->GetMethodID(packetClass, "<init>", "(B)V"), 35);
        auto getter = e->GetMethodID(packetClass, "method_11470", "()B");
        require(e->CallByteMethod(packet, getter) == 35 && !e->ExceptionCheck() && statuses == 0,
                "Removed totem effect still receives events");
        auto shared = e->NewGlobalRef(packet);
        bool workerOkay = false;
        std::thread worker([&] {
            JNIEnv *w = nullptr;
            vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void **>(&w), nullptr);
            workerOkay = w->CallByteMethod(shared, getter) == 35 && !w->ExceptionCheck();
            vm->DetachCurrentThread();
        });
        worker.join();
        e->DeleteGlobalRef(shared);
        require(workerOkay && statuses == 0, "Worker status read touched game state");
        e->CallByteMethod(packet, getter);
        require(statuses == 0, "Totem effect was not removed");
        vanta::modules::bloom::interested = true;
        vanta::core::pumpGameFrame();
        auto statusField = e->GetFieldID(packetClass, "field_12174", "B");
        e->SetByteField(packet, statusField, 3);
        require(e->CallByteMethod(packet, getter) == 3 && statuses == 0,
                "Removed death effect still receives events");
        auto fog = e->FindClass("net/minecraft/class_758");
        auto fogObject = e->NewObject(fog, e->GetMethodID(fog, "<init>", "()V"));
        e->CallVoidMethod(fogObject, e->GetMethodID(fog, "method_71108", "()V"));
        require(worldFrames == 1, "World effect hook did not run independently");
        e->CallByteMethod(packet, getter);
        require(statuses == 0, "Removed observer notified");
        std::cout << "Runtime passed: independent frame installation, six rendered swings, offhand, exception/matrix "
                     "restoration, removed death and totem effects, thread gate and world hook.\n";
        } catch (const std::exception &ex) {
        std::cerr << "Visual runtime test exception: " << ex.what() << "\n";
        if (e && e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
        result = 1;
    }
    vanta::modules::itemshader::shutdown_hook();
    JNIHook_Shutdown();
    shutdownCache();
    if (vm) {
        vm->DestroyJavaVM();
    }
    return result;
}
