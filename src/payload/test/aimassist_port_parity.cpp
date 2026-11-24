#include "modules/combat/aimassist/simple/Controllers.h"
#include <windows.h>
#include <jni.h>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
double sample(std::uint64_t &state, double minimum, double maximum) {
    std::uint64_t bits = (state += 0x9e3779b97f4a7c15ull);
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ull;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebull;
    bits ^= bits >> 31;
    const double result = minimum + (maximum - minimum) * (static_cast<double>(bits >> 11) * 0x1p-53);
    return result < maximum ? result : std::nextafter(maximum, minimum);
}
} // namespace
int main() {
    JavaVM *vm = nullptr;
    try {
        const std::filesystem::path path{WRATH_TEST_JVM_DLL};
        const auto bin = path.parent_path().parent_path();
        SetDllDirectoryW(bin.c_str());
        LoadLibraryW((bin / L"jli.dll").c_str());
        auto library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(library != nullptr, "Cannot load JVM");
        using Create = jint(JNICALL *)(JavaVM **, void **, void *);
        auto create = reinterpret_cast<Create>(GetProcAddress(library, "JNI_CreateJavaVM"));
        std::string classpath = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
        JavaVMOption options[] = {{classpath.data(), nullptr}};
        JavaVMInitArgs args{JNI_VERSION_1_8, 1, options, JNI_FALSE};
        JNIEnv *env = nullptr;
        require(create && create(&vm, reinterpret_cast<void **>(&env), &args) == JNI_OK, "Cannot create JVM");
        const auto ref = env->FindClass("net/minecraft/PortReference");
        require(ref != nullptr, "Java oracle not found");
        const auto reset = env->GetStaticMethodID(ref, "reset", "(J)V");
        const auto simple = env->GetStaticMethodID(ref, "simple", "(DDDDDDZ)[I");
        const auto manual = env->GetStaticMethodID(ref, "manual", "(DDDDZD)Z");
        const auto round = env->GetStaticMethodID(ref, "round", "(D)J");
        require(reset && simple && manual && round, "Java oracle methods missing");
        for (double value : {0.49999999999999994, 0.5, -0.5, -1.5, 1.5, -2.5, 0x1p63, -0x1p63, 0x1p64, -0x1p64,
                             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
            require(readableaim::javaRound(value) == env->CallStaticLongMethod(ref, round, value),
                    "Java rounding boundary mismatch");
        }
        std::mt19937_64 inputs{0x89a43de97ull};
        auto between = [&](double minimum, double maximum) {
            return std::uniform_real_distribution<double>(minimum, maximum)(inputs);
        };
        int updates = 0;
        for (int test = 0; test < 2000; ++test) {
            std::uint64_t state = inputs();
            env->CallStaticVoidMethod(ref, reset, std::bit_cast<jlong>(state));
            readableaim::SimpleAimController controller{[&](double lo, double hi) { return sample(state, lo, hi); }};
            for (int step = 0; step < 6; ++step) {
                const double x = between(-1200, 1200), y = between(-600, 600);
                const double tx = step % 3 == 0 ? x : between(-1200, 1200);
                const double ty = step % 3 == 0 ? y : between(-600, 600);
                const double speed = between(1, 100), response = between(1, 100);
                const bool consume = step % 2 != 0;
                auto expected = static_cast<jintArray>(env->CallStaticObjectMethod(
                    ref, simple, x, y, tx, ty, speed, response, static_cast<jboolean>(consume)));
                require(!env->ExceptionCheck(), "Java simple oracle failed");
                controller.updateRotation(x, y, tx, ty, speed, response);
                require((expected != nullptr) == controller.hasPendingRotation(), "Pending rotation mismatch");
                if (consume) {
                    const auto actual = controller.consumeRotation();
                    if (expected) {
                        jint values[2];
                        env->GetIntArrayRegion(expected, 0, 2, values);
                        require(actual && actual->horizontalUnits == values[0] && actual->verticalUnits == values[1],
                                "Simple differs from original Java mod");
                    } else
                        require(!actual, "Dead-zone state mismatch");
                }
                if (expected)
                    env->DeleteLocalRef(expected);
                ++updates;
            }
        }
        for (int test = 0; test < 100; ++test) {
            env->CallStaticVoidMethod(ref, reset, static_cast<jlong>(test));
            readableaim::ManualAimOverride override;
            for (int step = 0; step < 120; ++step) {
                const double x = between(-90, 90), y = between(-50, 50);
                const double seconds = step % 31 == 0 ? 0.5 : 1.0 / (30 + step);
                const double mx = between(-3, 3), my = between(-3, 3);
                const bool vertical = step % 2;
                const bool released =
                    env->CallStaticBooleanMethod(ref, manual, x, y, mx, my, static_cast<jboolean>(vertical), seconds);
                require(released == override.shouldSuspend(x, y, mx, my, vertical, seconds),
                        "Manual override differs from original Java mod");
                ++updates;
            }
        }
        require(!env->ExceptionCheck(), "JNI oracle exception");
        env->DeleteLocalRef(ref);
        vm->DestroyJavaVM();
        vm = nullptr;
        std::cout << "PASS: " << updates << " C++/Java controller comparisons and Java rounding boundaries\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (vm)
            vm->DestroyJavaVM();
        return 1;
    }
}
