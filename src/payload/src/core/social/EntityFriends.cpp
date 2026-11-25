#include "core/social/EntityFriends.h"

#include "core/social/FriendList.h"
#include "jvm/JniHelper.h"
#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>

namespace dt::social {

namespace {

/* Intermediary names, grepped out of tools/genmaps/.cache/<ver>/yarn_merged.tiny
   and identical on every version in the supported range (1.21 .. 1.21.11):

     class_1657  PlayerEntity  method_7334  getGameProfile ()Lcom/mojang/authlib/GameProfile;
     class_1297  Entity        method_5722  isTeammate (Lnet/minecraft/class_1297;)Z
     class_1297  Entity        method_5780  getScoreboardTeam ()Lnet/minecraft/class_270;
     class_270   AbstractTeam  method_1203  getColor ()Lnet/minecraft/class_124;
     class_124   Formatting    method_542   isColor ()Z

   The same pair spectate.cpp resolves a watched player's name with. */
struct Cache {
    jclass    player_cls     = nullptr;   // global ref, class_1657 -- for IsInstanceOf
    jmethodID get_profile    = nullptr;
    jmethodID prof_name      = nullptr;

    jmethodID is_teammate    = nullptr;   // method_5722 / isAlliedTo
    jmethodID get_team       = nullptr;   // method_5780 / getTeam
    jmethodID team_color     = nullptr;   // method_1203 / getColor
    jmethodID color_is_color = nullptr;   // method_542 / isColor

    jmethodID get_display_name = nullptr; // method_5476 / getDisplayName
    jmethodID get_name         = nullptr; // method_5477 / getName
    jmethodID text_get_string  = nullptr; // method_10851 / getString
    jmethodID text_get_style   = nullptr; // method_10866 / getStyle
    jmethodID style_get_color  = nullptr; // method_10973 / getColor
    jmethodID color_get_rgb    = nullptr; // method_27716 / getValue
};
Cache C{};

/* 0 = not resolved yet (retry, with backoff), 1 = ready, 2 = unavailable. */
std::atomic<int> g_state{ 0 };

std::mutex g_resolve_mx;
double     g_next_try = 0.0;           // ms; guarded by g_resolve_mx

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* @the retry is BACKED OFF, and it has to be.

   Before a world exists the classes are simply not loaded, so this fails and
   must be retried -- but findClassAnyLoader's miss path is not cheap. It climbs
   to the root ThreadGroup, enumerates every thread in the VM and calls
   ClassLoader.loadClass on each one's context loader, throwing and catching a
   Java exception per miss. Retrying that on every query, from a 1 kHz worker,
   would cost more than the feature. Two seconds between attempts makes the
   failed state free. */
bool ensure(JNIEnv* env)
{
    const int st = g_state.load(std::memory_order_acquire);
    if (st == 1) return true;
    if (st == 2) return false;

    std::lock_guard<std::mutex> lk(g_resolve_mx);

    const int st2 = g_state.load(std::memory_order_relaxed);
    if (st2 == 1) return true;
    if (st2 == 2) return false;

    const double now = now_ms();
    if (now < g_next_try) return false;
    g_next_try = now + 2000.0;

    jclass player = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1657");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!player) {
        player = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/world/entity/player/Player");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!player) return false;         /* no world yet; stay at 0 and retry later */

    C.get_profile = env->GetMethodID(player, "method_7334",
                                     "()Lcom/mojang/authlib/GameProfile;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!C.get_profile) {
        C.get_profile = env->GetMethodID(player, "getGameProfile",
                                         "()Lcom/mojang/authlib/GameProfile;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (jclass gp = ::dt::jvm::findClassAnyLoader(env, "com/mojang/authlib/GameProfile")) {
        C.prof_name = env->GetMethodID(gp, "getName", "()Ljava/lang/String;");
        env->DeleteLocalRef(gp);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // isTeammate on Entity (inherited by PlayerEntity)
    C.is_teammate = env->GetMethodID(player, "method_5722",
                                     "(Lnet/minecraft/class_1297;)Z");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!C.is_teammate) {
        C.is_teammate = env->GetMethodID(player, "isAlliedTo",
                                         "(Lnet/minecraft/world/entity/Entity;)Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // getScoreboardTeam on Entity (inherited by PlayerEntity)
    C.get_team = env->GetMethodID(player, "method_5780",
                                  "()Lnet/minecraft/class_270;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!C.get_team) {
        C.get_team = env->GetMethodID(player, "getTeam",
                                      "()Lnet/minecraft/world/scores/Team;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // AbstractTeam.getColor
    if (jclass team_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_270")) {
        C.team_color = env->GetMethodID(team_cls, "method_1203", "()Lnet/minecraft/class_124;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(team_cls);
    }
    if (!C.team_color) {
        if (jclass team_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/world/scores/Team")) {
            C.team_color = env->GetMethodID(team_cls, "getColor", "()Lnet/minecraft/ChatFormatting;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(team_cls);
        }
    }

    // Formatting.isColor
    if (jclass fmt_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_124")) {
        C.color_is_color = env->GetMethodID(fmt_cls, "method_542", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(fmt_cls);
    }
    if (!C.color_is_color) {
        if (jclass fmt_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/ChatFormatting")) {
            C.color_is_color = env->GetMethodID(fmt_cls, "isColor", "()Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(fmt_cls);
        }
    }

    // DisplayName & Name on Entity (class_1297 / class_1657)
    C.get_display_name = env->GetMethodID(player, "method_5476", "()Lnet/minecraft/class_2561;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!C.get_display_name) {
        C.get_display_name = env->GetMethodID(player, "getDisplayName", "()Lnet/minecraft/network/chat/Component;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    C.get_name = env->GetMethodID(player, "method_5477", "()Lnet/minecraft/class_2561;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!C.get_name) {
        C.get_name = env->GetMethodID(player, "getName", "()Lnet/minecraft/network/chat/Component;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Text / Component.getString & getStyle
    if (jclass text_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2561")) {
        C.text_get_string = env->GetMethodID(text_cls, "method_10851", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!C.text_get_string) {
            C.text_get_string = env->GetMethodID(text_cls, "getString", "()Ljava/lang/String;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        C.text_get_style = env->GetMethodID(text_cls, "method_10866", "()Lnet/minecraft/class_2583;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(text_cls);
    }
    if (!C.text_get_string) {
        if (jclass text_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/network/chat/Component")) {
            C.text_get_string = env->GetMethodID(text_cls, "getString", "()Ljava/lang/String;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            C.text_get_style = env->GetMethodID(text_cls, "getStyle", "()Lnet/minecraft/network/chat/Style;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(text_cls);
        }
    }

    // Style.getColor
    if (jclass style_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2583")) {
        C.style_get_color = env->GetMethodID(style_cls, "method_10973", "()Lnet/minecraft/class_5251;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(style_cls);
    }
    if (!C.style_get_color) {
        if (jclass style_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/network/chat/Style")) {
            C.style_get_color = env->GetMethodID(style_cls, "getColor", "()Lnet/minecraft/network/chat/TextColor;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(style_cls);
        }
    }

    // TextColor.getRgb
    if (jclass color_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/class_5251")) {
        C.color_get_rgb = env->GetMethodID(color_cls, "method_27716", "()I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(color_cls);
    }
    if (!C.color_get_rgb) {
        if (jclass color_cls = ::dt::jvm::findClassAnyLoader(env, "net/minecraft/network/chat/TextColor")) {
            C.color_get_rgb = env->GetMethodID(color_cls, "getValue", "()I");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(color_cls);
        }
    }

    jclass global = nullptr;
    if (C.get_profile && C.prof_name) {
        global = static_cast<jclass>(env->NewGlobalRef(player));
        /* NewGlobalRef throws OutOfMemoryError rather than returning null on
           some VMs, and this is called from inside a detour where a pending
           exception would surface in unrelated game code. */
        if (env->ExceptionCheck()) { env->ExceptionClear(); global = nullptr; }
    }

    env->DeleteLocalRef(player);

    if (!global) {
        VLOG_WARN("friends", "PlayerEntity.getGameProfile did not resolve -- "
                             "friends cannot be matched to entities");
        g_state.store(2, std::memory_order_release);
        return false;
    }

    C.player_cls = global;
    VLOG_INFO("friends", "entity matching armed");
    g_state.store(1, std::memory_order_release);
    return true;
}

} // namespace

bool isFriend(JNIEnv* env, jobject entity)
{
    if (!env || !entity) return false;

    /* One relaxed atomic load. For a user with no friends -- which is most of
       them, most of the time -- the whole feature costs exactly this. */
    FriendList& fl = FriendList::get();
    if (fl.empty()) return false;

    if (!ensure(env)) return false;

    /* A non-player has no GameProfile and calling method_7334 on one throws. */
    if (env->IsInstanceOf(entity, C.player_cls) != JNI_TRUE) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    bool answer = false;

    if (jobject profile = env->CallObjectMethod(entity, C.get_profile)) {
        if (jstring js = static_cast<jstring>(env->CallObjectMethod(profile, C.prof_name))) {
            if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                /* Compared as a view: contains() folds case as it hashes, so
                   there is no std::string built here and nothing allocated on
                   a path that can run thousands of times a second. */
                answer = fl.contains(std::string_view(c));
                env->ReleaseStringUTFChars(js, c);
            }
            env->DeleteLocalRef(js);
        }
        env->DeleteLocalRef(profile);
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    return answer;
}

bool isTeammate(JNIEnv* env, jobject local_player, jobject target)
{
    if (!env || !local_player || !target) return false;
    if (env->IsSameObject(local_player, target) == JNI_TRUE) return true;

    if (!ensure(env)) return false;

    // Fast path: vanilla Entity.isTeammate(Entity other)
    if (!C.is_teammate) {
        if (jclass local_cls = env->GetObjectClass(local_player)) {
            C.is_teammate = env->GetMethodID(local_cls, "method_5722", "(Lnet/minecraft/class_1297;)Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!C.is_teammate) {
                C.is_teammate = env->GetMethodID(local_cls, "isAlliedTo", "(Lnet/minecraft/world/entity/Entity;)Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(local_cls);
        }
    }

    if (C.is_teammate) {
        jboolean allied = env->CallBooleanMethod(local_player, C.is_teammate, target);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (allied == JNI_TRUE) {
            return true;
        }
    }

    // Secondary path: compare scoreboard teams / team colors
    if (C.get_team) {
        jobject team1 = env->CallObjectMethod(local_player, C.get_team);
        if (env->ExceptionCheck()) { env->ExceptionClear(); team1 = nullptr; }

        if (team1) {
            jobject team2 = env->CallObjectMethod(target, C.get_team);
            if (env->ExceptionCheck()) { env->ExceptionClear(); team2 = nullptr; }

            if (team2) {
                if (env->IsSameObject(team1, team2) == JNI_TRUE) {
                    env->DeleteLocalRef(team2);
                    env->DeleteLocalRef(team1);
                    return true;
                }

                if (C.team_color) {
                    jobject color1 = env->CallObjectMethod(team1, C.team_color);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); color1 = nullptr; }

                    if (color1) {
                        jobject color2 = env->CallObjectMethod(team2, C.team_color);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); color2 = nullptr; }

                        if (color2) {
                            if (env->IsSameObject(color1, color2) == JNI_TRUE) {
                                bool is_real_color = true;
                                if (C.color_is_color) {
                                    jboolean b = env->CallBooleanMethod(color1, C.color_is_color);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); b = JNI_FALSE; }
                                    is_real_color = (b == JNI_TRUE);
                                }
                                env->DeleteLocalRef(color2);
                                env->DeleteLocalRef(color1);
                                env->DeleteLocalRef(team2);
                                env->DeleteLocalRef(team1);
                                return is_real_color;
                            }
                            env->DeleteLocalRef(color2);
                        }
                        env->DeleteLocalRef(color1);
                    }
                }
                env->DeleteLocalRef(team2);
            }
            env->DeleteLocalRef(team1);
        }
    }

    // Tertiary path: Auto-detection via DisplayName / Nametag color & prefixes
    auto extract_color = [](const char* str) -> char {
        if (!str) return '\0';
        for (size_t i = 0; str[i] != '\0'; ++i) {
            // UTF-8 section sign: 0xC2 0xA7 followed by color code
            if (static_cast<unsigned char>(str[i]) == 0xC2 && static_cast<unsigned char>(str[i+1]) == 0xA7) {
                char code = str[i+2];
                if (code >= 'A' && code <= 'Z') code = static_cast<char>(code + 32);
                if ((code >= '1' && code <= '6') || code == '9' || (code >= 'a' && code <= 'e')) {
                    return code;
                }
            }
            // Byte 0xA7
            if (static_cast<unsigned char>(str[i]) == 0xA7 && str[i+1] != '\0') {
                char code = str[i+1];
                if (code >= 'A' && code <= 'Z') code = static_cast<char>(code + 32);
                if ((code >= '1' && code <= '6') || code == '9' || (code >= 'a' && code <= 'e')) {
                    return code;
                }
            }
            // Prefix tags [R], [B], [G], [Y]
            if (str[i] == '[') {
                if (str[i+1] == 'R' && str[i+2] == ']') return 'c';
                if (str[i+1] == 'B' && str[i+2] == ']') return '9';
                if (str[i+1] == 'G' && str[i+2] == ']') return 'a';
                if (str[i+1] == 'Y' && str[i+2] == ']') return 'e';
            }
        }
        return '\0';
    };

    auto get_name_text = [&](jobject entity) -> jobject {
        jobject text = nullptr;
        if (C.get_display_name) {
            text = env->CallObjectMethod(entity, C.get_display_name);
            if (env->ExceptionCheck()) { env->ExceptionClear(); text = nullptr; }
        }
        if (!text && C.get_name) {
            text = env->CallObjectMethod(entity, C.get_name);
            if (env->ExceptionCheck()) { env->ExceptionClear(); text = nullptr; }
        }
        return text;
    };

    jobject text1 = get_name_text(local_player);
    jobject text2 = get_name_text(target);

    if (text1 && text2) {
        // Check TextColor RGB from Style
        if (C.text_get_style && C.style_get_color && C.color_get_rgb) {
            jobject st1 = env->CallObjectMethod(text1, C.text_get_style);
            if (env->ExceptionCheck()) { env->ExceptionClear(); st1 = nullptr; }
            jobject st2 = env->CallObjectMethod(text2, C.text_get_style);
            if (env->ExceptionCheck()) { env->ExceptionClear(); st2 = nullptr; }
            if (st1 && st2) {
                jobject col1 = env->CallObjectMethod(st1, C.style_get_color);
                if (env->ExceptionCheck()) { env->ExceptionClear(); col1 = nullptr; }
                jobject col2 = env->CallObjectMethod(st2, C.style_get_color);
                if (env->ExceptionCheck()) { env->ExceptionClear(); col2 = nullptr; }
                if (col1 && col2) {
                    jint rgb1 = env->CallIntMethod(col1, C.color_get_rgb);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); rgb1 = 0; }
                    jint rgb2 = env->CallIntMethod(col2, C.color_get_rgb);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); rgb2 = 0; }
                    env->DeleteLocalRef(col2);
                    env->DeleteLocalRef(col1);
                    env->DeleteLocalRef(st2);
                    env->DeleteLocalRef(st1);
                    if (rgb1 != 0 && rgb1 == rgb2 && rgb1 != 0xFFFFFF && rgb1 != 0xAAAAAA && rgb1 != 0x555555) {
                        env->DeleteLocalRef(text2);
                        env->DeleteLocalRef(text1);
                        return true;
                    }
                } else {
                    if (col1) env->DeleteLocalRef(col1);
                    if (col2) env->DeleteLocalRef(col2);
                    env->DeleteLocalRef(st2);
                    env->DeleteLocalRef(st1);
                }
            } else {
                if (st1) env->DeleteLocalRef(st1);
                if (st2) env->DeleteLocalRef(st2);
            }
        }

        // Check DisplayName String for color codes (§c, §9, etc.)
        if (C.text_get_string) {
            jstring js1 = static_cast<jstring>(env->CallObjectMethod(text1, C.text_get_string));
            if (env->ExceptionCheck()) { env->ExceptionClear(); js1 = nullptr; }
            jstring js2 = static_cast<jstring>(env->CallObjectMethod(text2, C.text_get_string));
            if (env->ExceptionCheck()) { env->ExceptionClear(); js2 = nullptr; }
            if (js1 && js2) {
                const char* c1 = env->GetStringUTFChars(js1, nullptr);
                const char* c2 = env->GetStringUTFChars(js2, nullptr);
                char team1_col = extract_color(c1);
                char team2_col = extract_color(c2);
                if (c1) env->ReleaseStringUTFChars(js1, c1);
                if (c2) env->ReleaseStringUTFChars(js2, c2);
                env->DeleteLocalRef(js2);
                env->DeleteLocalRef(js1);
                if (team1_col != '\0' && team1_col == team2_col) {
                    env->DeleteLocalRef(text2);
                    env->DeleteLocalRef(text1);
                    return true;
                }
            } else {
                if (js1) env->DeleteLocalRef(js1);
                if (js2) env->DeleteLocalRef(js2);
            }
        }
    }
    if (text1) env->DeleteLocalRef(text1);
    if (text2) env->DeleteLocalRef(text2);

    if (env->ExceptionCheck()) env->ExceptionClear();
    return false;
}

} // namespace dt::social
