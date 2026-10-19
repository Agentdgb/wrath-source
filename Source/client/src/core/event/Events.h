#pragma once
#include "core/event/Event.h"

#include <cstdint>
#include <jni.h>
#include <string>

namespace vanta::events {

using vanta::Cancellable;
using vanta::Event;

// ── Tick ──────────────────────────────────────────────────────────────────────
struct PreTickEvent   : Event {};
struct PostTickEvent  : Event {};
struct GameLoopEvent  : Event { float deltaMs; };

// ── Packets ───────────────────────────────────────────────────────────────────
struct PacketSendEvent    : Event, Cancellable {
    JNIEnv* env;
    jobject packet;   // local reference — valid for the duration of dispatch only
    void   replace(jobject newPacket) { packet = newPacket; replaced = true; }
    bool   replaced = false;
};
struct PacketReceiveEvent : Event, Cancellable {
    JNIEnv* env;
    jobject packet;
    void   replace(jobject newPacket) { packet = newPacket; replaced = true; }
    bool   replaced = false;
};

// ── Rendering ─────────────────────────────────────────────────────────────────
struct Render2DEvent  : Event { float scaledW, scaledH, partialTicks; };
struct Render3DEvent  : Event {
    const float* modelView;
    const float* projection;
    float        partialTicks;
    double       camX, camY, camZ;
};

// ── Input ─────────────────────────────────────────────────────────────────────
struct KeyPressEvent   : Event { int virtualKey; bool down, repeat; bool handled = false; };
struct MouseButtonEvent: Event { int button; bool down; bool handled = false; };
struct MoveInputEvent  : Event {
    float forward = 0.f, strafe = 0.f;
    bool  jumping = false, sneaking = false, sprinting = false;
};

// Pre-movement packet publish; modules mutate to cancel/adjust the outgoing
// motion or attach a rotation for silent aim.
struct UpdateWalkingPlayerEvent : Event, Cancellable {
    double x, y, z;
    float  yaw, pitch;
    bool   onGround;
    bool   suppressPacket = false;   // drop the packet entirely (Blink-style)
};

struct JumpEvent        : Event, Cancellable { float motionY; };
struct AttackEntityEvent: Event, Cancellable {
    JNIEnv* env; jobject target;
    void retarget(jobject newTarget) { target = newTarget; retargeted = true; }
    bool retargeted = false;
};

struct WorldChangeEvent : Event { JNIEnv* env; jobject oldWorld; jobject newWorld; };
struct ScreenOpenEvent  : Event, Cancellable { JNIEnv* env; jobject screen; };
struct ChatSendEvent    : Event, Cancellable { std::string message; };
struct ChatReceiveEvent : Event, Cancellable { JNIEnv* env; jobject component; std::string plain; };

// Fired when a module is enabled/disabled — arraylist + notifications rely on it.
struct ModuleToggleEvent : Event { const char* moduleName; bool nowEnabled; };

} // namespace vanta::events
