#pragma once
#include <jni.h>
#include <string>
#include <string_view>

namespace vanta::jvm::packet {

// Typed views over jobject packets. Each helper returns std::nullopt-equivalent
// (empty string / default) when the runtime class does not match the canonical
// type — modules branch on that instead of stringly comparing class names.

bool         instanceOf(JNIEnv* env, jobject packet, std::string_view canonName);
std::string  className(JNIEnv* env, jobject packet);

// PlayerMoveC2SPacket family — spoof onGround for NoFall.
bool moveIsOnGround(JNIEnv* env, jobject packet);
void moveSetOnGround(JNIEnv* env, jobject packet, bool value);

// Chat family — extract outgoing text.
std::string chatOutgoingMessage(JNIEnv* env, jobject packet);
std::string chatOutgoingCommand(JNIEnv* env, jobject packet);

// Incoming chat family — extract Text component as plain string.
std::string chatIncomingPlainText(JNIEnv* env, jobject packet);

} // namespace vanta::jvm::packet
