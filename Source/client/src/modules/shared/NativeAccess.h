#pragma once
#include <windows.h>
#include <cstdint>
#include "jvm/JniHelper.h"
#include "NativeNames.h"
#include "util/Hash.h"
#include <ankerl/unordered_dense.h>
#include <stdexcept>
#include <type_traits>
#include <cmath>

namespace vanta::modules::native {
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
// Used only on the game/render thread. Local frames bound every JNI allocation.
struct Frame {
    JNIEnv* e;
    explicit Frame(JNIEnv* env) : e(env) { if(e->PushLocalFrame(256)<0) { e->ExceptionClear(); throw Failure("JNI local frame"); } }
    ~Frame() { if(e->ExceptionCheck()) e->ExceptionClear(); e->PopLocalFrame(nullptr); }
};
class Access {
    ankerl::unordered_dense::map<std::uint64_t, jclass> classes;
    ankerl::unordered_dense::map<std::uint64_t, jmethodID> methods;
    ankerl::unordered_dense::map<std::uint64_t, jfieldID> fields;
public:
    JNIEnv* e = nullptr;
    ~Access() { jvm::JniScope s; if(s) for(auto& [k,c]:classes) s.get()->DeleteGlobalRef(c); }
    void check() { if(e->ExceptionCheck()) { e->ExceptionClear(); throw Failure("Java interaction failed"); } }
    std::string named(std::string s) {
        for(auto& [from,to]:names) {
            std::size_t at=0;
            const std::string token="L"+from+";", replacement="L"+to+";";
            while((at=s.find(token,at))!=std::string::npos) { s.replace(at,token.size(),replacement); at+=replacement.size(); }
        }
        return s;
    }
    const char* name(const char* id) { auto i=names.find(id); return i==names.end()?id:i->second.c_str(); }
    jclass cls(const char* id) {
        const auto key = fnv1a64(id);
        auto it=classes.find(key); if(it!=classes.end()) return it->second;
        jclass c=jvm::findClassAnyLoader(e,id); if(e->ExceptionCheck()) e->ExceptionClear();
        if(!c) { c=jvm::findClassAnyLoader(e,name(id)); if(e->ExceptionCheck()) e->ExceptionClear(); }
        if(!c) throw Failure(std::string("Missing class: ")+id);
        auto g=static_cast<jclass>(e->NewGlobalRef(c)); e->DeleteLocalRef(c); check();
        if(!g) throw Failure("JNI global reference"); classes[key]=g; return g;
    }
    jmethodID method(jclass c,const char* id,const char* sig,bool stat=false) {
        // Include the declaring runtime class: inherited methods can have different overrides.
        const auto key = fnv1a64(id) ^ (fnv1a64(sig) * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<std::uintptr_t>(c) ^ (stat ? 1ull : 0ull);
        auto it=methods.find(key); if(it!=methods.end()) return it->second;
        auto m=stat?e->GetStaticMethodID(c,id,sig):e->GetMethodID(c,id,sig);
        if(e->ExceptionCheck()) e->ExceptionClear();
        if(!m) { auto ns=named(sig); m=stat?e->GetStaticMethodID(c,name(id),ns.c_str()):e->GetMethodID(c,name(id),ns.c_str()); if(e->ExceptionCheck()) e->ExceptionClear(); }
        if(!m) throw Failure(std::string("Missing method: ")+id+sig);
        methods[key]=m; return m;
    }
    jfieldID field(jclass c,const char* id,const char* sig,bool stat=false) {
        const auto key = fnv1a64(id) ^ (fnv1a64(sig) * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<std::uintptr_t>(c) ^ (stat ? 1ull : 0ull);
        auto it=fields.find(key); if(it!=fields.end()) return it->second;
        auto f=stat?e->GetStaticFieldID(c,id,sig):e->GetFieldID(c,id,sig);
        if(e->ExceptionCheck()) e->ExceptionClear();
        if(!f) { auto ns=named(sig); f=stat?e->GetStaticFieldID(c,name(id),ns.c_str()):e->GetFieldID(c,name(id),ns.c_str()); if(e->ExceptionCheck()) e->ExceptionClear(); }
        if(!f) throw Failure(std::string("Missing field: ")+id);
        fields[key]=f; return f;
    }
    template<class T=jobject,class... A> T call(jobject o,const char* owner,const char* id,const char* sig,A... args) {
        if(!o) throw Failure("Missing game object"); auto m=method(cls(owner),id,sig);
        if constexpr(std::is_same_v<T,void>) { e->CallVoidMethod(o,m,args...); check(); }
        else { T r{};
            if constexpr(std::is_same_v<T,jobject>) r=e->CallObjectMethod(o,m,args...);
            else if constexpr(std::is_same_v<T,jboolean>) r=e->CallBooleanMethod(o,m,args...);
            else if constexpr(std::is_same_v<T,jint>) r=e->CallIntMethod(o,m,args...);
            else if constexpr(std::is_same_v<T,jfloat>) r=e->CallFloatMethod(o,m,args...);
            else if constexpr(std::is_same_v<T,jdouble>) r=e->CallDoubleMethod(o,m,args...);
            check(); return r;
        }
    }
    jobject get(jobject o,const char* owner,const char* id,const char* sig) {
        if(!o) throw Failure("Missing game object"); auto f=field(cls(owner),id,sig); auto r=e->GetObjectField(o,f); check(); return r;
    }
    jobject constant(const char* owner,const char* id,const char* sig) {
        auto c=cls(owner); auto r=e->GetStaticObjectField(c,field(c,id,sig,true)); check(); return r;
    }
    template<class... A> jobject make(const char* owner,const char* sig,A... args) {
        auto c=cls(owner); auto r=e->NewObject(c,method(c,"<init>",sig),args...); check(); if(!r) throw Failure("Object construction failed"); return r;
    }
    jobject minecraft() {
        auto c=cls("net/minecraft/class_310"); auto r=e->CallStaticObjectMethod(c,method(c,"method_1551","()Lnet/minecraft/class_310;",true)); check(); return r;
    }
};
struct Vec { double x=0,y=0,z=0; };
inline double distance2(Vec a,Vec b) { return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z); }
inline Vec position(Access& a,jobject o,bool eye=false) { return {
    a.call<jdouble>(o,"net/minecraft/class_1297","method_23317","()D"),
    a.call<jdouble>(o,"net/minecraft/class_1297",eye?"method_23320":"method_23318","()D"),
    a.call<jdouble>(o,"net/minecraft/class_1297","method_23321","()D")}; }
inline bool focused() { DWORD pid=0; auto w=GetForegroundWindow(); if(!w) return false; GetWindowThreadProcessId(w,&pid); return pid==GetCurrentProcessId(); }
}
