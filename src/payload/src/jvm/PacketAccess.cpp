#include "jvm/PacketAccess.h"

#include "jvm/JniHelper.h"
#include "jvm/JniRef.h"
#include "mappings/MappingRegistry.h"

namespace dt::jvm::packet {

bool instanceOf(JNIEnv* env, jobject packet, std::string_view canonName) {
    if (!env || !packet) return false;
    jclass cls = findClassCanon(env, canonName);
    if (!cls) return false;
    return env->IsInstanceOf(packet, cls) == JNI_TRUE;
}

std::string className(JNIEnv* env, jobject packet) {
    if (!env || !packet) return {};
    jclass cls = env->GetObjectClass(packet);
    if (!cls) return {};
    LocalClass meta(env, env->GetObjectClass(cls));
    if (!meta) { env->DeleteLocalRef(cls); return {}; }
    jmethodID getName = env->GetMethodID(meta.get(), "getName", "()Ljava/lang/String;");
    if (!getName) { env->DeleteLocalRef(cls); return {}; }
    jstring js = static_cast<jstring>(env->CallObjectMethod(cls, getName));
    std::string s = toStdString(env, js);
    env->DeleteLocalRef(cls);
    if (js) env->DeleteLocalRef(js);
    return s;
}

bool moveIsOnGround(JNIEnv* env, jobject packet) {
    for (auto canon : {"PlayerMoveC2SPacket$PositionAndOnGround",
                       "PlayerMoveC2SPacket$LookAndOnGround",
                       "PlayerMoveC2SPacket$Full",
                       "PlayerMoveC2SPacket$OnGroundOnly",
                       "PlayerMoveC2SPacket"}) {
        if (instanceOf(env, packet, canon)) {
            return getBoolFieldCanon(env, packet, "PlayerMoveC2SPacket", "onGround") == JNI_TRUE;
        }
    }
    return false;
}

void moveSetOnGround(JNIEnv* env, jobject packet, bool value) {
    for (auto canon : {"PlayerMoveC2SPacket$PositionAndOnGround",
                       "PlayerMoveC2SPacket$LookAndOnGround",
                       "PlayerMoveC2SPacket$Full",
                       "PlayerMoveC2SPacket$OnGroundOnly",
                       "PlayerMoveC2SPacket"}) {
        if (instanceOf(env, packet, canon)) {
            setBoolFieldCanon(env, packet, "PlayerMoveC2SPacket", "onGround",
                              value ? JNI_TRUE : JNI_FALSE);
            return;
        }
    }
}

std::string chatOutgoingMessage(JNIEnv* env, jobject packet) {
    if (instanceOf(env, packet, "ChatMessageC2SPacket")) {
        jfieldID f = fieldCanon(env, "ChatMessageC2SPacket", "chatMessage");
        if (!f) return {};
        auto js = static_cast<jstring>(env->GetObjectField(packet, f));
        auto s  = toStdString(env, js);
        if (js) env->DeleteLocalRef(js);
        return s;
    }
    return {};
}

std::string chatOutgoingCommand(JNIEnv* env, jobject packet) {
    if (instanceOf(env, packet, "CommandExecutionC2SPacket")) {
        jfieldID f = fieldCanon(env, "CommandExecutionC2SPacket", "command");
        if (!f) return {};
        auto js = static_cast<jstring>(env->GetObjectField(packet, f));
        auto s  = toStdString(env, js);
        if (js) env->DeleteLocalRef(js);
        return s;
    }
    return {};
}

std::string chatIncomingPlainText(JNIEnv* env, jobject packet) {
    // Extract the Text component field from any of the receive packet variants,
    // then call .getString() on it (canonical Text#getString / Component#getString).
    for (auto tuple : std::initializer_list<std::pair<const char*, const char*>>{
        {"GameMessageS2CPacket",           "content"},
        {"PlayerChatMessageS2CPacket",     "unsignedContent"},
        {"ProfilelessChatMessageS2CPacket","message"},
    }) {
        if (!instanceOf(env, packet, tuple.first)) continue;
        jobject text = getObjectFieldCanon(env, packet, tuple.first, tuple.second);
        if (!text) continue;
        jstring js = static_cast<jstring>(callObjectCanon(env, text, "Text", "getString").release());
        std::string s = toStdString(env, js);
        env->DeleteLocalRef(text);
        if (js) env->DeleteLocalRef(js);
        return s;
    }
    return {};
}

} // namespace dt::jvm::packet
