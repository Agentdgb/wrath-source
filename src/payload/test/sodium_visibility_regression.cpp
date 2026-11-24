#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "modules/render/chams/chams_cull.h"
#include "util/DeskLog.h"
#include <filesystem>
#include <iostream>
#include <jnihook.h>
#include <stdexcept>
#include <unordered_set>
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
jclass findClassAnyLoader(JNIEnv *e, const char *n) {
    auto c = e->FindClass(n);
    if (e->ExceptionCheck())
        e->ExceptionClear();
    return c;
}
} // namespace vanta::jvm
namespace vanta::desklog {
void logf(Channel, const char *, ...) {}
void logf_site(Channel, const void *, unsigned, const char *, ...) {}
void section(Channel, const char *) {}
void jni_lookup(Channel, const char *, const char *, const char *, const char *, bool) {}
} // namespace vanta::desklog
namespace vanta::modules::chams::dbg {
vanta::desklog::Channel ch() { return {}; }
} // namespace vanta::modules::chams::dbg
namespace vanta::modules::chams::gl {
bool player = false;
void set_rendering_player(bool value) { player = value; }
} // namespace vanta::modules::chams::gl
namespace {
jclass rendererClass = nullptr;
jmethodID legacyOriginal = nullptr;
jboolean JNICALL legacy(JNIEnv *e, jobject self, jobject entity, jobject frustum, jdouble x, jdouble y, jdouble z) {
    return e->CallNonvirtualBooleanMethod(self, rendererClass, legacyOriginal, entity, frustum, x, y, z);
}
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **) {
    JavaVM *vm = nullptr;
    JNIEnv *e = nullptr;
    int result = 0;
    try {
        auto path = std::filesystem::path(WRATH_TEST_JVM_DLL);
        auto binDir = path.parent_path().parent_path();
        SetDllDirectoryW(binDir.c_str());
        LoadLibraryW((binDir / L"jli.dll").c_str());
        auto library = LoadLibraryW(path.c_str());
        if (!library) library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(library != nullptr, "Load JVM");
        using Create = jint(JNICALL *)(JavaVM **, void **, void *);
        auto create = reinterpret_cast<Create>(GetProcAddress(library, "JNI_CreateJavaVM"));
        std::string cp = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
        JavaVMOption options[] = {{cp.data(), nullptr}, {const_cast<char *>("-Xverify:all"), nullptr}};
        JavaVMInitArgs args{JNI_VERSION_1_8, 2, options, JNI_FALSE};
        require(create && create(&vm, reinterpret_cast<void **>(&e), &args) == JNI_OK, "Create JVM");
        vanta::jvm::ctx().vm.store(vm);
        namespace cull = vanta::modules::chams::cull;
        auto entityClass = e->FindClass("net/minecraft/class_1297"),
             playerClass = e->FindClass("net/minecraft/class_1657"),
             dispatcherClass = e->FindClass("net/minecraft/class_898");
        rendererClass = e->FindClass("net/minecraft/class_897");
        const char *signature = "(Lnet/minecraft/class_1297;Lnet/minecraft/class_4604;DDD)Z";
        auto render = e->GetMethodID(rendererClass, "method_3933", signature);
        auto shouldRender = e->GetMethodID(dispatcherClass, "method_3950", signature);
        auto renderer = e->NewObject(rendererClass, e->GetMethodID(rendererClass, "<init>", "()V"));
        auto dispatcher = e->NewObject(dispatcherClass, e->GetMethodID(dispatcherClass, "<init>", "()V"));
        auto player = e->NewObject(playerClass, e->GetMethodID(playerClass, "<init>", "()V"));
        auto mob = e->NewObject(entityClass, e->GetMethodID(entityClass, "<init>", "()V"));
        auto id = e->GetFieldID(entityClass, "id", "I"), visible = e->GetFieldID(entityClass, "visible", "Z");
        e->SetIntField(player, id, 11);
        e->SetIntField(mob, id, 22);
        require(!e->ExceptionCheck(), "Fixture binding failure");
        require(e->CallBooleanMethod(renderer, render, player, nullptr, 0., 0., 0.) && !e->ExceptionCheck(),
                "Baseline Sodium render failed");
        if (argc > 1) {
            require(JNIHook_Init(vm) == JNIHOOK_OK, "Initialize legacy reproducer");
            require(JNIHook_Attach(render, reinterpret_cast<void *>(&legacy), &legacyOriginal) == JNIHOOK_OK,
                    "Attach legacy reproducer");
            e->CallBooleanMethod(renderer, render, player, nullptr, 0., 0., 0.);
            auto exception = e->ExceptionOccurred();
            e->ExceptionClear();
            require(exception && e->IsInstanceOf(exception, e->FindClass("java/lang/NoSuchMethodError")),
                    "Legacy shape did not reproduce Sodium NoSuchMethodError");
            std::cout << "Legacy renderer copy reproduced Sodium's typed-receiver NoSuchMethodError.\n";
        } else {
            require(cull::install(e) && cull::install(e), "Visibility hook not idempotent");
            auto reflected = e->ToReflectedMethod(rendererClass, render, JNI_FALSE);
            auto methodClass = e->FindClass("java/lang/reflect/Method");
            auto modifiers = e->GetMethodID(methodClass, "getModifiers", "()I");
            require((e->CallIntMethod(reflected, modifiers) & 0x100) == 0, "Sodium renderer was rewritten to native");
            auto decision = [&](jobject entity) {
                bool value = e->CallBooleanMethod(dispatcher, shouldRender, entity, nullptr, 0., 0., 0.);
                require(!e->ExceptionCheck(), "Sodium visibility raised a Java exception");
                return value;
            };
            require(decision(player) && decision(mob), "Live entities lost Sodium visibility");
            e->SetBooleanField(player, visible, JNI_FALSE);
            cull::set_active(true);
            require(decision(player), "Chams visibility failed");
            e->SetBooleanField(mob, visible, JNI_FALSE);
            require(!decision(mob), "Chams forced a non-player");
            cull::set_active(false);
            require(!decision(player), "Inactive Chams overrode Sodium");
            e->SetBooleanField(player, visible, JNI_TRUE);
            for (int frame = 0; frame < 1000; ++frame)
                require(decision(player), "Repeated Sodium render failed");
            require(JNIHook_Shutdown() == JNIHOOK_OK, "Shared visibility hook did not restore");
            require(decision(player), "Restored dispatcher failed");
            cull::shutdown();
            std::cout << "Sodium visibility passed: original renderer untouched, live/mob culling, Chams "
                         "coexistence, disable, 1000 frames and detach under -Xverify:all.\n";
        }
    } catch (const std::exception &ex) {
        if (e && e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
        std::cerr << "Sodium visibility test exception: " << ex.what() << "\n";
        result = 1;
    }
    JNIHook_Shutdown();
    if (vm) {
        vm->DestroyJavaVM();
    }
    return result;
}
