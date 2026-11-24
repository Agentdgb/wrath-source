// Optional compatibility probe against the user's installed intermediary game.
// Takes a text file containing the classpath; it does not launch Minecraft.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <jni.h>
#include <jnihook.h>
#include <windows.h>
namespace {
jclass owner = nullptr;
jfieldID statusField = nullptr;
int observed = 0;
jbyte JNICALL getter(JNIEnv *e, jobject packet) {
    auto result = e->GetByteField(packet, statusField);
    if (!e->ExceptionCheck())
        ++observed;
    return result;
}
void require(bool okay, const char *message) {
    if (!okay)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::ifstream input(argv[1]);
    std::string classpath;
    std::getline(input, classpath);
    auto path = std::filesystem::path(WRATH_TEST_JVM_DLL);
    SetDllDirectoryW(path.parent_path().parent_path().c_str());
    auto lib = LoadLibraryW(path.c_str());
    require(lib != nullptr, "Load JVM");
    using Create = jint(JNICALL *)(JavaVM **, void **, void *);
    auto create = reinterpret_cast<Create>(GetProcAddress(lib, "JNI_CreateJavaVM"));
    std::string cp = "-Djava.class.path=" + classpath;
    JavaVMOption options[] = {{cp.data(), nullptr}, {const_cast<char *>("-Xverify:all"), nullptr}};
    JavaVMInitArgs args{JNI_VERSION_1_8, 2, options, JNI_FALSE};
    JavaVM *vm = nullptr;
    JNIEnv *e = nullptr;
    require(create && create(&vm, reinterpret_cast<void **>(&e), &args) == JNI_OK, "Create JVM");
    int result = 0;
    try {
        owner = static_cast<jclass>(e->NewGlobalRef(e->FindClass("net/minecraft/class_2663")));
        require(owner && !e->ExceptionCheck(), "Load actual 1.21.11 entity status packet");
        auto packet = e->AllocObject(owner);
        auto field = e->GetFieldID(owner, "field_12174", "B");
        auto method = e->GetMethodID(owner, "method_11470", "()B");
        require(packet && field && method && !e->ExceptionCheck(), "Resolve actual status fields/getter");
        e->SetByteField(packet, field, 35);
        require(e->CallByteMethod(packet, method) == 35 && !e->ExceptionCheck(), "Original actual getter");
        require(JNIHook_Init(vm) == JNIHOOK_OK, "Init JNIHook");
        statusField = field;
        auto attached = JNIHook_Attach(method, reinterpret_cast<void *>(&getter), nullptr);
        if (attached != JNIHOOK_OK)
            std::cerr << "Attach code " << int(attached) << '\n';
        require(attached == JNIHOOK_OK, "Attach actual status getter");
        require(e->CallByteMethod(packet, method) == 35 && !e->ExceptionCheck() && observed == 1,
                "Actual hooked totem getter changed or failed");
        e->SetByteField(packet, field, 3);
        require(e->CallByteMethod(packet, method) == 3 && !e->ExceptionCheck() && observed == 2,
                "Actual hooked death getter changed or failed");
        require(JNIHook_Shutdown() == JNIHOOK_OK, "Detach actual status getter");
        require(e->CallByteMethod(packet, method) == 3 && !e->ExceptionCheck(), "Restored getter failed");
        std::cout << "Actual Minecraft 1.21.11 status getter passed: unchanged 35/3 values, native callback without "
                     "codec initialization, and detach.\n";
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        if (e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
        JNIHook_Shutdown();
        result = 1;
    }
    if (owner)
        e->DeleteGlobalRef(owner);
    vm->DestroyJavaVM();
    return result;
}
