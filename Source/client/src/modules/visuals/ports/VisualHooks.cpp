#include "PortJni.h"
#include "VisualPorts.h"
#include "core/NativeCallbacks.h"
#include "modules/render/chams/item_shader_hook.h"
#include "modules/visuals/bloom/Atmosphere.h"
#include "modules/visuals/bloom/Bloom.h"
#include "modules/visuals/bloom/DeathEffects.h"
#include "util/Log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <jnihook.h>
namespace vanta::modules::visualports {
namespace {
jclass heldOwner = nullptr, livingOwner = nullptr, fogOwner = nullptr;
jmethodID heldOriginal = nullptr, durationOriginal = nullptr, fogOriginal = nullptr;
bool triedSwing = false, triedWorld = false, worldReady = false;
std::atomic<bool> stopping{false};
thread_local bool covered = true;
thread_local bool combatLatched = false;
thread_local bool handActive = false;
thread_local bool wasSwinging = false;
thread_local std::chrono::steady_clock::time_point lastCombat{};
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
void JNICALL swingArm(JNIEnv *e, jobject self, jfloat progress, jobject matrices, jint side, jobject arm) {
    NativeCallback life;
    if (swing.enabled && handActive && covered && (!swing.combatOnly || combatLatched)) {
        if (e->PushLocalFrame(64) < 0)
            return;
        try {
            for (auto &op : math::swingPose(swing.style, progress, side, swing.intensity)) {
                if (op.axis < 0)
                    jni::translate(e, matrices, op.x, op.y, op.z);
                else
                    jni::rotate(e, matrices, op.x * math::deg, op.y * math::deg, op.z * math::deg);
            }
            e->PopLocalFrame(nullptr);
            return;
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
            e->PopLocalFrame(nullptr);
        }
    }
    if (heldOriginal)
        e->CallNonvirtualVoidMethod(self, heldOwner, heldOriginal, progress, matrices, side, arm);
}
jint JNICALL duration(JNIEnv *e, jobject self) {
    NativeCallback life;
    int base = e->CallNonvirtualIntMethod(self, livingOwner, durationOriginal);
    if (e->ExceptionCheck())
        return base;
    if (!swing.enabled && !swing.slow)
        return base;
    if (e->PushLocalFrame(24) < 0)
        return base;
    try {
        auto me = jni::player(e, jni::mc(e));
        if (e->IsSameObject(self, me)) {
            if (swing.slow)
                base = std::max(1, static_cast<int>(std::round(base / std::max(.1f, swing.speed))));
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
void beforeHand(JNIEnv *e, jobject hand) {
    handActive = swing.enabled;
    covered = false;
    if (!handActive || e->PushLocalFrame(32) < 0)
        return;
    try {
        auto c = jni::cls(e, "net/minecraft/class_1268");
        auto f = e->GetStaticFieldID(c, "field_5808", "Lnet/minecraft/class_1268;");
        jni::check(e);
        covered = swing.offhand || e->IsSameObject(hand, e->GetStaticObjectField(c, f));
        auto p = jni::player(e, jni::mc(e));
        if (p) {
            auto pc = jni::cls(e, "net/minecraft/class_1309");
            bool active = e->GetBooleanField(p, jni::field(e, pc, "field_6252", "Z"));
            bool looked = combat(e);
            auto now = std::chrono::steady_clock::now();
            if (active && !wasSwinging && looked)
                lastCombat = now;
            if (!active || !wasSwinging)
                combatLatched = !swing.combatOnly || looked || now - lastCombat < std::chrono::seconds(3);
            wasSwinging = active;
        }
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
    }
    e->PopLocalFrame(nullptr);
}
void afterHand() { handActive = false; }
void tick() {
    if (stopping.load(std::memory_order_acquire))
        return;
    if (!blurEnabled && !spirit.enabled && !swing.enabled && !swing.slow && !character.enabled && !bloom::needsWorld())
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
    if ((swing.enabled || swing.slow) && !triedSwing) {
        triedSwing = true;
        try {
            heldOwner = static_cast<jclass>(e->NewGlobalRef(jni::cls(e, "net/minecraft/class_759")));
            auto arm = jni::optional(e, heldOwner, "method_65816",
                                     "(FLnet/minecraft/class_4587;ILnet/minecraft/class_1306;)V");
            if (arm)
                JNIHook_Attach(arm, reinterpret_cast<void *>(&swingArm), &heldOriginal);
            livingOwner = static_cast<jclass>(e->NewGlobalRef(jni::cls(e, "net/minecraft/class_1309")));
            auto d = jni::method(e, livingOwner, "method_6028", "()I");
            JNIHook_Attach(d, reinterpret_cast<void *>(&duration), &durationOriginal);
            itemshader::install_hook();
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
        }
    }
    if ((blurEnabled || spirit.enabled || bloom::needsWorld()) && !triedWorld) {
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
    if (character.enabled)
        installCharacter(e);
    e->PopLocalFrame(nullptr);
}
const char *swingStatus() {
    return heldOriginal ? "Custom first-person swing animations" : "Swing render hook unavailable";
}
const char *worldStatus() { return worldReady ? "Native world effect" : "World render hook unavailable"; }
bool prepareShutdown() {
    stopping.store(true, std::memory_order_release);
    bloom::shutdown();
    blurEnabled = false;
    spirit.enabled = false;
    swing.enabled = false;
    swing.slow = false;
    character.enabled = false;
    jvm::JniScope s;
    return !s || prepareCharacterShutdown(s.get());
}
bool isStopping() { return stopping.load(std::memory_order_acquire); }
void shutdownCache() {
    jvm::JniScope s;
    if (!s)
        return;
    auto *e = s.get();
    bloom::freeAtmosphere(e);
    bloom::freeDeathHide(e);
    shutdownCharacter(e);
    for (auto *c : {&heldOwner, &livingOwner, &fogOwner})
        if (*c) {
            e->DeleteGlobalRef(*c);
            *c = nullptr;
        }
}
} // namespace vanta::modules::visualports
