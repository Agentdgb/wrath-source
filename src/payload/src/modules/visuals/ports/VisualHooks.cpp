#include "PortJni.h"
#include "VisualPorts.h"
#include "core/NativeCallbacks.h"
#include "modules/render/chams/item_shader_hook.h"
#include "modules/visuals/bloom/Bloom.h"
#include "util/Log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <jnihook.h>
namespace dt::modules::visualports {
namespace {
jclass livingOwner = nullptr, fogOwner = nullptr;
jmethodID durationOriginal = nullptr, fogOriginal = nullptr;
bool triedSwing = false, triedWorld = false, worldReady = false, swingReady = false;
std::atomic<bool> stopping{false};
static bool combatLatched = false;
static bool wasSwinging = false;
static std::chrono::steady_clock::time_point lastCombat{};
bool combat(JNIEnv *e) {
    auto client = jni::mc(e);
    auto looked = e->GetObjectField(
        client, jni::field(e, jni::cls(e, "net/minecraft/class_310"), "field_1692", "Lnet/minecraft/class_1297;"));
    if (!looked)
        return false;
    return (e->IsInstanceOf(looked, jni::cls(e, "net/minecraft/class_1657")) ||
            e->IsInstanceOf(looked, jni::cls(e, "net/minecraft/class_1569"))) &&
           e->CallBooleanMethod(looked, jni::method(e, jni::cls(e, "net/minecraft/class_1297"), "method_5805", "()Z"));
}
jint JNICALL duration(JNIEnv *e, jobject self) {
    NativeCallback life;
    int base = e->CallNonvirtualIntMethod(self, livingOwner, durationOriginal);
    if (e->ExceptionCheck())
        return base;
    if (!swing.enabled)
        return base;
    if (e->PushLocalFrame(24) < 0)
        return base;
    try {
        auto me = jni::player(e, jni::mc(e));
        if (e->IsSameObject(self, me)) {
            if (swing.enabled && (!swing.combatOnly || combatLatched))
                base = std::max(1, static_cast<int>(std::round(base / swing.tempo)));
        }
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
    }
    e->PopLocalFrame(nullptr);
    return base;
}
void JNICALL rotateFog(JNIEnv *e, jobject self) {
    NativeCallback life;
    if (fogOriginal)
        e->CallNonvirtualVoidMethod(self, fogOwner, fogOriginal);
    if (!e->ExceptionCheck())
        afterWorld(e);
}
} // namespace
bool beforeHand(JNIEnv *e, jobject player, jobject hand, float progress, jobject matrices) {
    if (!swing.enabled || isStopping() || !player || !hand || !matrices || !std::isfinite(progress) || progress <= 0 ||
        progress >= 1 || e->ExceptionCheck())
        return false;
    if (e->PushLocalFrame(64) < 0)
        return false;
    bool pushed = false;
    jmethodID pop = nullptr;
    try {
        auto handClass = jni::cls(e, "net/minecraft/class_1268");
        auto mainField = e->GetStaticFieldID(handClass, "field_5808", "Lnet/minecraft/class_1268;");
        jni::check(e);
        bool main = e->IsSameObject(hand, e->GetStaticObjectField(handClass, mainField));
        if (!main && !swing.offhand) {
            e->PopLocalFrame(nullptr);
            return false;
        }
        auto living = jni::cls(e, "net/minecraft/class_1309");
        if (e->CallBooleanMethod(player, jni::method(e, living, "method_6115", "()Z"))) {
            e->PopLocalFrame(nullptr);
            return false;
        }
        bool active = e->GetBooleanField(player, jni::field(e, living, "field_6252", "Z"));
        bool looked = !swing.combatOnly || combat(e);
        auto now = std::chrono::steady_clock::now();
        if (active && !wasSwinging && looked)
            lastCombat = now;
        if (!active || !wasSwinging)
            combatLatched = looked || now - lastCombat < std::chrono::seconds(3);
        wasSwinging = active;
        if (swing.combatOnly && !combatLatched) {
            e->PopLocalFrame(nullptr);
            return false;
        }
        auto arm = e->CallObjectMethod(player, jni::method(e, living, "method_6068", "()Lnet/minecraft/class_1306;"));
        auto armClass = jni::cls(e, "net/minecraft/class_1306");
        auto rightField = e->GetStaticFieldID(armClass, "field_6183", "Lnet/minecraft/class_1306;");
        jni::check(e);
        bool right = e->IsSameObject(arm, e->GetStaticObjectField(armClass, rightField));
        int side = (main ? right : !right) ? 1 : -1;
        auto matrixClass = jni::cls(e, "net/minecraft/class_4587");
        auto push = jni::method(e, matrixClass, "method_22903", "()V");
        pop = jni::method(e, matrixClass, "method_22909", "()V");
        e->CallVoidMethod(matrices, push);
        jni::check(e);
        pushed = true;
        for (auto &op : math::swingPose(swing.style, progress, side, swing.intensity)) {
            if (op.axis < 0)
                jni::translate(e, matrices, op.x, op.y, op.z);
            else
                jni::rotate(e, matrices, op.x * math::deg, op.y * math::deg, op.z * math::deg);
        }
        e->PopLocalFrame(nullptr);
        return true;
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        if (pushed && pop)
            e->CallVoidMethod(matrices, pop);
        if (e->ExceptionCheck())
            e->ExceptionClear();
        e->PopLocalFrame(nullptr);
        return false;
    }
}
void afterHand(JNIEnv *e, jobject matrices, bool transformed) {
    if (!transformed)
        return;
    auto exception = e->ExceptionOccurred();
    if (exception)
        e->ExceptionClear();
    try {
        auto c = jni::cls(e, "net/minecraft/class_4587");
        auto pop = jni::method(e, c, "method_22909", "()V");
        e->CallVoidMethod(matrices, pop);
        e->DeleteLocalRef(c);
    } catch (...) {
    }
    if (exception) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        e->Throw(exception);
        e->DeleteLocalRef(exception);
    }
}
void tick() {
    if (stopping.load(std::memory_order_acquire))
        return;
    if (!swing.enabled && !bloom::needsWorld())
        return;
    jvm::JniScope scope;
    if (!scope)
        return;
    auto *e = scope.get();
    if (e->PushLocalFrame(128) < 0)
        return;
    const auto init = JNIHook_Init(jvm::ctx().vm.load());
    if (init != JNIHOOK_OK && init != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        e->PopLocalFrame(nullptr);
        return;
    }
    if ((swing.enabled) && !triedSwing) {
        triedSwing = true;
        try {
            livingOwner = static_cast<jclass>(e->NewGlobalRef(jni::cls(e, "net/minecraft/class_1309")));
            auto d = jni::method(e, livingOwner, "method_6028", "()I");
            JNIHook_Attach(d, reinterpret_cast<void *>(&duration), &durationOriginal);
            swingReady = itemshader::install_hook();
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
        }
    }
    if (bloom::needsWorld() && !triedWorld) {
        triedWorld = true;
        try {
            fogOwner = static_cast<jclass>(e->NewGlobalRef(jni::cls(e, "net/minecraft/class_758")));
            auto r = jni::optional(e, fogOwner, "method_71108", "()V");
            if (r)
                worldReady = JNIHook_Attach(r, reinterpret_cast<void *>(&rotateFog), &fogOriginal) == JNIHOOK_OK;
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
        }
    }
    e->PopLocalFrame(nullptr);
}
const char *swingStatus() {
    return swingReady ? "Custom first-person swing animations" : "Swing render hook unavailable";
}
const char *worldStatus() { return worldReady ? "Native world effect" : "World render hook unavailable"; }
bool prepareShutdown() {
    stopping.store(true, std::memory_order_release);
    bloom::shutdown();
    swing.enabled = false;
    return true;
}
bool isStopping() { return stopping.load(std::memory_order_acquire); }
void shutdownCache() {
    jvm::JniScope s;
    if (!s)
        return;
    auto *e = s.get();
    for (auto *c : {&livingOwner, &fogOwner})
        if (*c) {
            e->DeleteGlobalRef(*c);
            *c = nullptr;
        }
}
} // namespace dt::modules::visualports
