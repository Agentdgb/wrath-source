#include "hook/bridge/HookBridge.h"

#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "util/Log.h"

namespace vanta::hook::bridge {

// Each native trampoline builds a stack-local event, dispatches, returns the
// cancel/continue code. Zero heap allocation on the hot path.
// Bound directly via JNIEnv::RegisterNatives — never exported in the PE header.

static jint JNICALL bridge_onClientTick(JNIEnv*, jclass) {
    events::PreTickEvent evt;
    EventBus::get().post(evt);
    return kBridgeContinue;
}

static jint JNICALL bridge_onGameLoop(JNIEnv*, jclass, jfloat deltaMs) {
    events::GameLoopEvent evt{{}, deltaMs};
    EventBus::get().post(evt);
    return kBridgeContinue;
}

static jint JNICALL bridge_onKeyPress(JNIEnv*, jclass, jint vk, jint down, jint repeat, jint /*mods*/) {
    events::KeyPressEvent evt{{}, vk, down != 0, repeat != 0, false};
    EventBus::get().post(evt);
    return evt.handled ? kBridgeCancel : kBridgeContinue;
}

static jint JNICALL bridge_onMouseButton(JNIEnv*, jclass, jint btn, jint down) {
    events::MouseButtonEvent evt{{}, btn, down != 0, false};
    EventBus::get().post(evt);
    return evt.handled ? kBridgeCancel : kBridgeContinue;
}

static jint JNICALL bridge_onPacketSend(JNIEnv* env, jclass, jobject packet) {
    events::PacketSendEvent evt{{}, {}, env, packet};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onPacketReceive(JNIEnv* env, jclass, jobject packet) {
    events::PacketReceiveEvent evt{{}, {}, env, packet};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onRenderWorldPre(JNIEnv*, jclass, jfloat partial) {
    events::Render3DEvent evt{{}, nullptr, nullptr, partial, 0.0, 0.0, 0.0};
    EventBus::get().post(evt);
    return kBridgeContinue;
}

static jint JNICALL bridge_onRenderHud(JNIEnv*, jclass, jfloat scaledW, jfloat scaledH, jfloat partial) {
    events::Render2DEvent evt{{}, scaledW, scaledH, partial};
    EventBus::get().post(evt);
    return kBridgeContinue;
}

static jint JNICALL bridge_onPlayerMove(JNIEnv*, jclass,
        jdouble x, jdouble y, jdouble z, jfloat yaw, jfloat pitch, jint onGround) {
    events::UpdateWalkingPlayerEvent evt{{}, {}, x, y, z, yaw, pitch, onGround != 0, false};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onJump(JNIEnv*, jclass, jfloat motionY) {
    events::JumpEvent evt{{}, {}, motionY};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onAttackEntity(JNIEnv* env, jclass, jobject target) {
    events::AttackEntityEvent evt{{}, {}, env, target};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onChatSend(JNIEnv* env, jclass, jstring msg) {
    const char* p = env->GetStringUTFChars(msg, nullptr);
    events::ChatSendEvent evt;
    if (p) { evt.message.assign(p); env->ReleaseStringUTFChars(msg, p); }
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

static jint JNICALL bridge_onScreenOpen(JNIEnv* env, jclass, jobject screen) {
    events::ScreenOpenEvent evt{{}, {}, env, screen};
    return EventBus::get().post(evt) ? kBridgeContinue : kBridgeCancel;
}

// ── RegisterNatives table ────────────────────────────────────────────────────
void registerNatives(JNIEnv* env, jclass cls) {
    if (!env || !cls) return;

    static const JNINativeMethod kMethods[] = {
        {(char*)"onClientTick",     (char*)"()I",                                      (void*)bridge_onClientTick},
        {(char*)"onGameLoop",       (char*)"(F)I",                                     (void*)bridge_onGameLoop},
        {(char*)"onKeyPress",       (char*)"(IIII)I",                                  (void*)bridge_onKeyPress},
        {(char*)"onMouseButton",    (char*)"(II)I",                                    (void*)bridge_onMouseButton},
        {(char*)"onPacketSend",     (char*)"(Ljava/lang/Object;)I",                    (void*)bridge_onPacketSend},
        {(char*)"onPacketReceive",  (char*)"(Ljava/lang/Object;)I",                    (void*)bridge_onPacketReceive},
        {(char*)"onRenderWorldPre", (char*)"(F)I",                                     (void*)bridge_onRenderWorldPre},
        {(char*)"onRenderHud",      (char*)"(FFF)I",                                   (void*)bridge_onRenderHud},
        {(char*)"onPlayerMove",     (char*)"(DDDFFI)I",                                (void*)bridge_onPlayerMove},
        {(char*)"onJump",           (char*)"(F)I",                                     (void*)bridge_onJump},
        {(char*)"onAttackEntity",   (char*)"(Ljava/lang/Object;)I",                    (void*)bridge_onAttackEntity},
        {(char*)"onChatSend",       (char*)"(Ljava/lang/String;)I",                    (void*)bridge_onChatSend},
        {(char*)"onScreenOpen",     (char*)"(Ljava/lang/Object;)I",                    (void*)bridge_onScreenOpen},
    };
    jint r = env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(*kMethods));
    if (r != JNI_OK) {
        VLOG_ERR("bridge", "RegisterNatives failed: {}", r);
    } else {
        VLOG_INFO("bridge", "HookBridge natives registered ({} entries)", sizeof(kMethods)/sizeof(*kMethods));
    }
}

} // namespace vanta::hook::bridge
