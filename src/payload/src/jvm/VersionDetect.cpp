#include "jvm/VersionDetect.h"

#include "jvm/JniHelper.h"

#include "jvm/JniRef.h"
#include "mappings/Bootstrap.h"
#include "util/Log.h"

#include <mutex>

namespace dt::jvm {

namespace {

RuntimeInfo    g_runtime;
std::once_flag g_once;

std::string tryReflectSharedConstants(JNIEnv* env) {
    // Try each of the known obf names for SharedConstants across 1.21.x
    // (bootstrap table lives in src/mappings/Bootstrap.cpp).
    for (const char* clsName : mappings::bootstrapSharedConstantsCandidates()) {
        // NOT env->FindClass: this thread is one we attached, so FindClass
        // resolves through the system loader, which under Fabric does not
        // have Minecraft on it. That is why this whole path was failing and
        // the launcher's own version was being used instead.
        jclass cls = findClassAnyLoader(env, clsName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
        if (!cls) continue;

        // Look for `getGameVersion()Lnet/minecraft/GameVersion;` or its obf name.
        // At bootstrap-time we don't have the mapping registry yet; instead we
        // scan static methods returning any object with a `getName()Ljava/lang/String;`
        // — brittle but only used until MappingRegistry takes over.
        for (const char* mname : {"getGameVersion", "b", "a"}) {
            jmethodID mid = env->GetStaticMethodID(cls, mname, "()Lnet/minecraft/GameVersion;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
            if (!mid) continue;
            jobject gv = env->CallStaticObjectMethod(cls, mid);
            if (!gv || env->ExceptionCheck()) { env->ExceptionClear(); continue; }

            jclass gvCls = env->GetObjectClass(gv);
            jmethodID getName = env->GetMethodID(gvCls, "getName", "()Ljava/lang/String;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
            if (!getName) { env->DeleteLocalRef(gv); env->DeleteLocalRef(gvCls); continue; }

            jstring js = static_cast<jstring>(env->CallObjectMethod(gv, getName));
            std::string v = toStdString(env, js);
            env->DeleteLocalRef(js);
            env->DeleteLocalRef(gv);
            env->DeleteLocalRef(gvCls);
            env->DeleteLocalRef(cls);
            return v;
        }
        env->DeleteLocalRef(cls);
    }
    return {};
}

std::string trySystemProperty(JNIEnv* env, const char* key) {
    jclass    sys = env->FindClass("java/lang/System");
    if (!sys || env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    jmethodID gp  = env->GetStaticMethodID(sys, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring   jk  = env->NewStringUTF(key);
    jstring   jv  = static_cast<jstring>(env->CallStaticObjectMethod(sys, gp, jk));
    std::string v = toStdString(env, jv);
    env->DeleteLocalRef(jk);
    env->DeleteLocalRef(sys);
    if (jv) env->DeleteLocalRef(jv);
    return v;
}

} // namespace

RuntimeInfo detectRuntime(JNIEnv* env) {
    RuntimeInfo r;
    if (!env) return r;

    r.loader    = detectLoader(env);

    /* Order matters, and the previous order was wrong.
       "minecraft.launcher.version" is the LAUNCHER's own version, not the
       game's — it reported 0.18.2 on a 1.21.x install, which then bound the
       mapping registry to the wrong table. Under Fabric, "fabric.gameVersion"
       IS the Minecraft version, so it goes first among the properties, and the
       launcher property is not consulted at all. */
    r.mcVersion = tryReflectSharedConstants(env);
    if (r.mcVersion.empty()) r.mcVersion = trySystemProperty(env, "fabric.gameVersion");
    if (r.mcVersion.empty()) r.mcVersion = trySystemProperty(env, "minecraft.version");

    // Anything that is not x.y[.z] is not a Minecraft version — a launcher
    // string that slipped through must not silently pick a mapping table.
    if (!r.mcVersion.empty() && r.mcVersion.rfind("1.", 0) != 0) {
        VLOG_WARN("ver", "ignoring implausible MC version '{}'", r.mcVersion);
        r.mcVersion.clear();
    }

    if (r.mcVersion.empty()) {
        VLOG_WARN("ver", "Could not detect MC version; defaulting to 1.21.11 mapping set");
        r.mcVersion = "1.21.11";
    }

    r.isDev = trySystemProperty(env, "fabric.development") == "true";

    std::call_once(g_once, [&]{ g_runtime = r; });
    VLOG_INFO("ver", "Detected MC {} on loader={} (dev={})", r.mcVersion, loaderName(r.loader), r.isDev);
    return r;
}

const RuntimeInfo& currentRuntime() { return g_runtime; }

} // namespace dt::jvm
