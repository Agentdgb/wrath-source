#include "SwordReplaceResources.h"
#include "SwordReplace.h"
#include "core/NativeCallbacks.h"
#include "core/UnloadSafety.h"
#include "jvm/JniHelper.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace Hook { void ensure_tls_for_this_thread(); }
namespace vanta::modules::swordreplace {
namespace {
constexpr const char* id = "wrath:cosmetics";
constexpr const char* materials[] = {"wooden", "stone", "copper", "iron", "golden", "diamond", "netherite"};
enum Role { Provider=1, Factory=2, Pack=3, Supplier=4, ModelRouter=5 };
std::atomic<bool> requested{false};
std::atomic<int> requestedModel{0};
std::atomic<bool> equipped{false};
std::atomic<int> cosmeticIndex{0};
jclass bridge = nullptr;
jobject loader = nullptr, provider = nullptr, packInfo = nullptr, pending = nullptr;
jobject itemManager=nullptr, originalGetter=nullptr, modelRouter=nullptr;
jfieldID getterField=nullptr;
jmethodID functionApply=nullptr;
bool routerMounted=false;
jfieldID keyField = nullptr;
bool initialized=false, appliedEnabled=false, queuedEnabled=false, stopping=false;
bool mounted=false, legacyCleaned=false;
int appliedModel=-1, queuedModel=-1;
std::chrono::steady_clock::time_point retryAt{};
std::string error;

void check(JNIEnv* e) { if(e->ExceptionCheck()) throw std::runtime_error("Minecraft could not load cosmetic resources"); }
jclass cls(JNIEnv* e, const char* name) {
    auto c=jvm::findClassAnyLoader(e,name); check(e);
    if(!c) throw std::runtime_error(std::string("Missing cosmetic interface: ")+name);
    return c;
}
jmethodID method(JNIEnv* e,jclass c,const char* n,const char* d) {
    auto m=e->GetMethodID(c,n,d); check(e);
    if(!m) throw std::runtime_error(std::string("Missing cosmetic method: ")+n);
    return m;
}
jmethodID staticMethod(JNIEnv* e,jclass c,const char* n,const char* d) {
    auto m=e->GetStaticMethodID(c,n,d); check(e); return m;
}
jobject constant(JNIEnv* e,const char* owner,const char* name,const char* desc) {
    auto c=cls(e,owner); auto f=e->GetStaticFieldID(c,name,desc); check(e);
    auto v=e->GetStaticObjectField(c,f); check(e); return v;
}
std::string utf(JNIEnv* e,jstring s) {
    if(!s) return {}; const char* p=e->GetStringUTFChars(s,nullptr); check(e);
    if(!p) throw std::runtime_error("Cannot read cosmetic identifier");
    std::string value(p); e->ReleaseStringUTFChars(s,p); return value;
}
jobject emptyOptional(JNIEnv* e) {
    auto c=cls(e,"java/util/Optional");
    return e->CallStaticObjectMethod(c,staticMethod(e,c,"empty","()Ljava/util/Optional;"));
}
jobject text(JNIEnv* e,const char* value) {
    auto c=cls(e,"net/minecraft/class_2561");
    return e->CallStaticObjectMethod(c,staticMethod(e,c,"method_43470","(Ljava/lang/String;)Lnet/minecraft/class_5250;"),e->NewStringUTF(value));
}
jobject list(JNIEnv* e) {
    auto c=cls(e,"java/util/ArrayList"); return e->NewObject(c,method(e,c,"<init>","()V"));
}
jlong handle(Role role,int index) { return static_cast<jlong>(role)*1000000+index; }
jobject proxy(JNIEnv* e,const char* interfaceName,Role role,int index) {
    auto handler=e->NewObject(bridge,method(e,bridge,"<init>","(J)V"),handle(role,index)); check(e);
    auto cc=cls(e,"java/lang/Class"); auto interfaces=e->NewObjectArray(1,cc,nullptr);
    e->SetObjectArrayElement(interfaces,0,cls(e,interfaceName));
    auto pc=cls(e,"java/lang/reflect/Proxy");
    auto result=e->CallStaticObjectMethod(pc,staticMethod(e,pc,"newProxyInstance",
        "(Ljava/lang/ClassLoader;[Ljava/lang/Class;Ljava/lang/reflect/InvocationHandler;)Ljava/lang/Object;"),loader,interfaces,handler);
    check(e); return result;
}
jobject profile(JNIEnv* e,int model) {
    auto factory=proxy(e,"net/minecraft/class_3288$class_7680",Factory,model);
    auto metadataClass=cls(e,"net/minecraft/class_3288$class_7679");
    auto features=cls(e,"net/minecraft/class_7699");
    auto featureSet=e->CallStaticObjectMethod(features,staticMethod(e,features,"method_45397","()Lnet/minecraft/class_7699;"));
    auto metadata=e->NewObject(metadataClass,method(e,metadataClass,"<init>",
        "(Lnet/minecraft/class_2561;Lnet/minecraft/class_3281;Lnet/minecraft/class_7699;Ljava/util/List;)V"),
        text(e,"Sword cosmetics"),constant(e,"net/minecraft/class_3281","field_14224","Lnet/minecraft/class_3281;"),featureSet,list(e));
    auto positionClass=cls(e,"net/minecraft/class_9225");
    auto position=e->NewObject(positionClass,method(e,positionClass,"<init>","(ZLnet/minecraft/class_3288$class_3289;Z)V"),JNI_FALSE,
        constant(e,"net/minecraft/class_3288$class_3289","field_14280","Lnet/minecraft/class_3288$class_3289;"),JNI_TRUE);
    auto pc=cls(e,"net/minecraft/class_3288");
    auto value=e->NewObject(pc,method(e,pc,"<init>",
        "(Lnet/minecraft/class_9224;Lnet/minecraft/class_3288$class_7680;Lnet/minecraft/class_3288$class_7679;Lnet/minecraft/class_9225;)V"),
        packInfo,factory,metadata,position);
    check(e); return value;
}
int resourceIndex(std::string_view path) {
    for(unsigned i=0;i<assetCount;++i) if(path==assets[i].path) return static_cast<int>(i);
    for(int i=0;i<20;++i) if(path==std::string("assets/minecraft/items/wrath_cosmetic/")+std::to_string(i)+".json") return static_cast<int>(assetCount)+i;
    return -1;
}
jobject supplier(JNIEnv* e,int model,int index) { return proxy(e,"net/minecraft/class_7367",Supplier,model*10000+index); }
jobject stream(JNIEnv* e,int model,int index) {
    std::string generated;
    const unsigned char* bytes=nullptr; unsigned size=0;
    if(index>=0 && index<static_cast<int>(assetCount)) { bytes=assets[index].bytes; size=assets[index].size; }
    else if(index>=static_cast<int>(assetCount) && index<static_cast<int>(assetCount)+20) {
        generated=std::string("{\"model\":{\"type\":\"minecraft:model\",\"model\":\"")+modelPaths[index-static_cast<int>(assetCount)]+"\"}}";
        bytes=reinterpret_cast<const unsigned char*>(generated.data()); size=static_cast<unsigned>(generated.size());
    }
    if(!bytes) return nullptr;
    auto data=e->NewByteArray(size); check(e);
    e->SetByteArrayRegion(data,0,size,reinterpret_cast<const jbyte*>(bytes));
    auto c=cls(e,"java/io/ByteArrayInputStream");
    auto result=e->NewObject(c,method(e,c,"<init>","([B)V"),data); check(e); return result;
}
bool clientResources(JNIEnv* e,jobject type) {
    return e->IsSameObject(type,constant(e,"net/minecraft/class_3264","field_14188","Lnet/minecraft/class_3264;"));
}
jobject dispatch(JNIEnv* e,jobject self,jobject object,jobject reflected,jobjectArray args) {
    const auto key=e->GetLongField(self,keyField);
    const int role=static_cast<int>(key/1000000), index=static_cast<int>(key%1000000);
    auto mc=cls(e,"java/lang/reflect/Method");
    auto name=utf(e,static_cast<jstring>(e->CallObjectMethod(reflected,method(e,mc,"getName","()Ljava/lang/String;"))));
    auto arg=[&](int i){ return e->GetObjectArrayElement(args,i); };
    if(name=="toString") return e->NewStringUTF("WrathCosmetics");
    if(name=="equals") {
        auto c=cls(e,"java/lang/Boolean");
        return e->CallStaticObjectMethod(c,staticMethod(e,c,"valueOf","(Z)Ljava/lang/Boolean;"),e->IsSameObject(object,arg(0)));
    }
    if(name=="hashCode") {
        auto c=cls(e,"java/lang/Integer");
        return e->CallStaticObjectMethod(c,staticMethod(e,c,"valueOf","(I)Ljava/lang/Integer;"),static_cast<jint>(key));
    }
    if(role==ModelRouter) {
        auto identifier=arg(0);
        if(equipped.load(std::memory_order_acquire) && identifier) {
            auto c=cls(e,"net/minecraft/class_2960");
            const auto ns=utf(e,static_cast<jstring>(e->CallObjectMethod(identifier,method(e,c,"method_12836","()Ljava/lang/String;"))));
            const auto path=utf(e,static_cast<jstring>(e->CallObjectMethod(identifier,method(e,c,"method_12832","()Ljava/lang/String;"))));
            bool sword=false;
            if(ns=="minecraft") for(const char* material:materials) if(path==std::string(material)+"_sword") { sword=true;break; }
            if(sword) {
                const auto cosmeticPath=std::string("wrath_cosmetic/")+std::to_string(cosmeticIndex.load(std::memory_order_relaxed));
                identifier=e->CallStaticObjectMethod(c,staticMethod(e,c,"method_60655","(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;"),e->NewStringUTF("minecraft"),e->NewStringUTF(cosmeticPath.c_str()));check(e);
            }
        }
        auto result=e->CallObjectMethod(originalGetter,functionApply,identifier);check(e);return result;
    }
    if(role==Provider) {
        if(requested.load(std::memory_order_acquire)) {
            auto consumer=arg(0); auto c=cls(e,"java/util/function/Consumer");
            e->CallVoidMethod(consumer,method(e,c,"accept","(Ljava/lang/Object;)V"),profile(e,requestedModel.load(std::memory_order_relaxed)));
            check(e);
        }
        return nullptr;
    }
    if(role==Factory) return proxy(e,"net/minecraft/class_3262",Pack,index);
    if(role==Supplier) return stream(e,index/10000,index%10000);
    if(role!=Pack) return nullptr;
    if(name=="method_56926") return e->NewLocalRef(packInfo);
    if(name=="method_14409") return e->NewStringUTF(id);
    if(name=="method_56929") return emptyOptional(e);
    if(name=="close" || name=="method_14407") return nullptr;
    if(name=="method_14406") {
        auto c=cls(e,"java/util/Collections");
        if(clientResources(e,arg(0))) return e->CallStaticObjectMethod(c,staticMethod(e,c,"singleton","(Ljava/lang/Object;)Ljava/util/Set;"),e->NewStringUTF("minecraft"));
        return e->CallStaticObjectMethod(c,staticMethod(e,c,"emptySet","()Ljava/util/Set;"));
    }
    if(name=="method_14410") {
        auto parts=static_cast<jobjectArray>(arg(0)); std::string path;
        for(jsize i=0;i<e->GetArrayLength(parts);++i) { if(i)path+='/';path+=utf(e,static_cast<jstring>(e->GetObjectArrayElement(parts,i))); }
        int found=resourceIndex(path); return found<0?nullptr:supplier(e,index,found);
    }
    if(name=="method_14405") {
        if(!clientResources(e,arg(0))) return nullptr;
        auto identifier=arg(1); auto c=cls(e,"net/minecraft/class_2960");
        auto ns=utf(e,static_cast<jstring>(e->CallObjectMethod(identifier,method(e,c,"method_12836","()Ljava/lang/String;"))));
        auto path=utf(e,static_cast<jstring>(e->CallObjectMethod(identifier,method(e,c,"method_12832","()Ljava/lang/String;"))));
        int found=resourceIndex("assets/"+ns+"/"+path); return found<0?nullptr:supplier(e,index,found);
    }
    if(name=="method_14408") {
        if(!clientResources(e,arg(0)) || utf(e,static_cast<jstring>(arg(1)))!="minecraft") return nullptr;
        const auto prefix=utf(e,static_cast<jstring>(arg(2)));
        auto consumer=arg(3); auto ic=cls(e,"net/minecraft/class_2960");
        auto identifier=staticMethod(e,ic,"method_60655","(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;");
        auto cc=cls(e,"java/util/function/BiConsumer"); auto accept=method(e,cc,"accept","(Ljava/lang/Object;Ljava/lang/Object;)V");
        auto emit=[&](std::string_view path,int n) {
            if(!path.starts_with("assets/minecraft/")) return;
            path.remove_prefix(17); if(!path.starts_with(prefix)) return;
            auto location=e->CallStaticObjectMethod(ic,identifier,e->NewStringUTF("minecraft"),e->NewStringUTF(std::string(path).c_str()));
            e->CallVoidMethod(consumer,accept,location,supplier(e,index,n)); check(e);
        };
        // Each callback gets its own frame so animated/resource-rich models
        // cannot exhaust a worker thread's JNI local-reference table.
        for(unsigned i=0;i<assetCount;++i) { e->PushLocalFrame(16); emit(assets[i].path,static_cast<int>(i));e->PopLocalFrame(nullptr); }
        for(int i=0;i<20;++i) { e->PushLocalFrame(16);emit(std::string("assets/minecraft/items/wrath_cosmetic/")+std::to_string(i)+".json",static_cast<int>(assetCount)+i);e->PopLocalFrame(nullptr); }
    }
    return nullptr;
}
jobject JNICALL invoke(JNIEnv* e,jobject self,jobject object,jobject reflected,jobjectArray args) {
    Hook::ensure_tls_for_this_thread();
    NativeCallback callback;
    if(e->PushLocalFrame(96)!=JNI_OK) return nullptr;
    jobject result=nullptr;
    try { result=dispatch(e,self,object,reflected,args); }
    catch(const std::exception& ex) {
        if(!e->ExceptionCheck()) { auto c=e->FindClass("java/lang/IllegalStateException");e->ThrowNew(c,ex.what()); }
    }
    return e->PopLocalFrame(result);
}
jobject minecraft(JNIEnv* e) {
    auto c=cls(e,"net/minecraft/class_310");
    auto value=e->CallStaticObjectMethod(c,staticMethod(e,c,"method_1551","()Lnet/minecraft/class_310;"));check(e);return value;
}
jobject manager(JNIEnv* e,jobject mc) {
    auto c=cls(e,"net/minecraft/class_310"); auto f=e->GetFieldID(c,"field_1715","Lnet/minecraft/class_3283;");check(e);
    auto value=e->GetObjectField(mc,f);check(e);return value;
}
void installRouter(JNIEnv* e) {
    if(routerMounted)return;
    auto mc=minecraft(e);auto mcClass=cls(e,"net/minecraft/class_310");
    auto field=e->GetFieldID(mcClass,"field_55226","Lnet/minecraft/class_10442;");check(e);
    auto instance=e->GetObjectField(mc,field);check(e);
    auto c=cls(e,"net/minecraft/class_10442");
    getterField=e->GetFieldID(c,"field_55334","Ljava/util/function/Function;");check(e);
    auto getter=e->GetObjectField(instance,getterField);check(e);
    if(!getter)throw std::runtime_error("Minecraft item models are not ready");
    auto fc=cls(e,"java/util/function/Function");functionApply=method(e,fc,"apply","(Ljava/lang/Object;)Ljava/lang/Object;");
    itemManager=e->NewGlobalRef(instance);originalGetter=e->NewGlobalRef(getter);
    modelRouter=e->NewGlobalRef(proxy(e,"java/util/function/Function",ModelRouter,0));check(e);
    e->SetObjectField(itemManager,getterField,modelRouter);check(e);routerMounted=true;
}
void removeRouter(JNIEnv* e) {
    equipped.store(false,std::memory_order_release);
    if(!routerMounted)return;
    auto current=e->GetObjectField(itemManager,getterField);check(e);
    if(e->IsSameObject(current,modelRouter))e->SetObjectField(itemManager,getterField,originalGetter);
    else if(!e->IsSameObject(current,originalGetter)) {
        e->DeleteLocalRef(current);
        throw std::runtime_error("Another item model provider still references the cosmetic router");
    }
    e->DeleteLocalRef(current);check(e);routerMounted=false;
}
void providerSet(JNIEnv* e,jobject packs,bool add) {
    auto pc=cls(e,"net/minecraft/class_3283"); auto field=e->GetFieldID(pc,"field_14227","Ljava/util/Set;");check(e);
    auto old=e->GetObjectField(packs,field); auto sc=cls(e,"java/util/LinkedHashSet");
    auto set=e->NewObject(sc,method(e,sc,"<init>","(Ljava/util/Collection;)V"),old);
    e->CallBooleanMethod(set,method(e,sc,add?"add":"remove","(Ljava/lang/Object;)Z"),provider);check(e);
    e->SetObjectField(packs,field,set);check(e);
}
void initialize(JNIEnv* e,jobject mc,jobject packs) {
    if(mounted) return;
    if(!bridge) {
    auto cc=cls(e,"java/lang/Class");
    auto mcClass=cls(e,"net/minecraft/class_310");
    loader=e->NewGlobalRef(e->CallObjectMethod(mcClass,method(e,cc,"getClassLoader","()Ljava/lang/ClassLoader;")));check(e);
    const auto* data=findAsset("native/CosmeticBridge.class");
    if(!data)throw std::runtime_error("Missing embedded cosmetic adapter");
    auto carrier=jvm::findClassAnyLoader(e,"dev/wrath/CosmeticBridge");
    if(!carrier) {
        if(e->ExceptionCheck())e->ExceptionClear();
        carrier=e->DefineClass("dev/wrath/CosmeticBridge",loader,reinterpret_cast<const jbyte*>(data->bytes),data->size);check(e);
    }
    bridge=static_cast<jclass>(e->NewGlobalRef(carrier));
    JNINativeMethod native{const_cast<char*>("invoke"),const_cast<char*>("(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"),reinterpret_cast<void*>(&invoke)};
    if(e->RegisterNatives(bridge,&native,1)!=JNI_OK) throw std::runtime_error("Could not connect cosmetic adapter to DLL");
    keyField=e->GetFieldID(bridge,"key","J");check(e);
    }
    if(!packInfo) {
    auto info=cls(e,"net/minecraft/class_9224");
    packInfo=e->NewGlobalRef(e->NewObject(info,method(e,info,"<init>","(Ljava/lang/String;Lnet/minecraft/class_2561;Lnet/minecraft/class_5352;Ljava/util/Optional;)V"),
        e->NewStringUTF(id),text(e,"Cosmetics"),constant(e,"net/minecraft/class_5352","field_25347","Lnet/minecraft/class_5352;"),emptyOptional(e)));check(e);
    }
    if(!provider)provider=e->NewGlobalRef(proxy(e,"net/minecraft/class_3285",Provider,0));check(e);
    providerSet(e,packs,true);
    mounted=true;
}
void removeLegacyFile(JNIEnv* e) {
    if(legacyCleaned) return;
    if(e->PushLocalFrame(16)!=JNI_OK)return;
    auto c=cls(e,"net/minecraft/class_310");
    auto field=e->GetFieldID(c,"field_1697","Ljava/io/File;");
    if(!field) { e->ExceptionClear();e->PopLocalFrame(nullptr);legacyCleaned=true;return; }
    auto directory=e->GetObjectField(minecraft(e),field);
    auto fc=cls(e,"java/io/File");
    auto file=e->NewObject(fc,method(e,fc,"<init>","(Ljava/io/File;Ljava/lang/String;)V"),directory,
        e->NewStringUTF("resourcepacks/wrath-swordreplace-godweer.zip"));
    const bool existed=e->CallBooleanMethod(file,method(e,fc,"exists","()Z"));
    const bool removed=existed && e->CallBooleanMethod(file,method(e,fc,"delete","()Z"));
    if(removed) {
        auto packs=manager(e,minecraft(e)); auto pc=cls(e,"net/minecraft/class_3283");
        e->CallVoidMethod(packs,method(e,pc,"method_14445","()V"));
    }
    legacyCleaned=!existed || removed;
    if(e->ExceptionCheck())e->ExceptionClear();
    e->PopLocalFrame(nullptr);
}
bool managed(std::string_view name) { return name==id || name=="file/wrath-swordreplace-godweer.zip"; }
void reload(JNIEnv* e,bool enabled,int model) {
    auto mc=minecraft(e); auto packs=manager(e,mc);
    if(enabled) initialize(e,mc,packs);
    requestedModel.store(model,std::memory_order_relaxed);
    requested.store(enabled,std::memory_order_release);
    auto pc=cls(e,"net/minecraft/class_3283");
    e->CallVoidMethod(packs,method(e,pc,"method_14445","()V"));check(e);
    auto current=e->CallObjectMethod(packs,method(e,pc,"method_29210","()Ljava/util/Collection;"));check(e);
    auto collection=cls(e,"java/util/Collection");
    auto array=static_cast<jobjectArray>(e->CallObjectMethod(current,method(e,collection,"toArray","()[Ljava/lang/Object;")));check(e);
    auto selected=list(e);auto lc=cls(e,"java/util/ArrayList");auto add=method(e,lc,"add","(Ljava/lang/Object;)Z");
    bool changed=enabled || appliedEnabled;
    for(jsize i=0;i<e->GetArrayLength(array);++i) {
        auto name=static_cast<jstring>(e->GetObjectArrayElement(array,i));
        if(managed(utf(e,name))) { changed=true;continue; }
        e->CallBooleanMethod(selected,add,name);check(e);
    }
    if(enabled) e->CallBooleanMethod(selected,add,e->NewStringUTF(id));
    if(!changed) { initialized=true;appliedEnabled=false;appliedModel=model;return; }
    e->CallVoidMethod(packs,method(e,pc,"method_14447","(Ljava/util/Collection;)V"),selected);check(e);
    auto mcClass=cls(e,"net/minecraft/class_310");
    auto future=e->CallObjectMethod(mc,method(e,mcClass,"method_1521","()Ljava/util/concurrent/CompletableFuture;"));check(e);
    if(!future)throw std::runtime_error("Minecraft did not start cosmetic reload");
    pending=e->NewGlobalRef(future);queuedEnabled=enabled;queuedModel=model;
}
} // namespace

const char* prepareResources(JNIEnv* e,bool enabled,int model) {
    if(stopping) enabled=false;
    cosmeticIndex.store(model,std::memory_order_relaxed);
    equipped.store(enabled,std::memory_order_release);
    // The resource set is loaded once. Changing the cosmetic only redirects
    // an ordinary Function lookup to one of the 20 already-baked models.
    enabled=stopping?false:(enabled || appliedEnabled || (pending && queuedEnabled));
    model=0;
    if(pending) {
        auto c=e->GetObjectClass(pending);
        auto done=e->GetMethodID(c,"isDone","()Z");
        auto failed=e->GetMethodID(c,"isCompletedExceptionally","()Z");
        const bool complete=e->CallBooleanMethod(pending,done);
        const bool failure=e->CallBooleanMethod(pending,failed);
        e->DeleteLocalRef(c);
        if(!complete) return "Loading cosmetic";
        e->DeleteGlobalRef(pending);pending=nullptr;
        if(failure) { initialized=false;error="Minecraft could not load the cosmetic";retryAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);return error.c_str(); }
        initialized=true;appliedEnabled=queuedEnabled;appliedModel=queuedModel;
    }
    if(initialized && appliedEnabled==enabled && (!enabled || appliedModel==model)) {
        if(appliedEnabled && !stopping) {
            if(e->PushLocalFrame(32)!=JNI_OK)return "Minecraft is busy";
            try { installRouter(e); }
            catch(const std::exception& ex) { if(e->ExceptionCheck())e->ExceptionClear();error=ex.what();e->PopLocalFrame(nullptr);return error.c_str(); }
            e->PopLocalFrame(nullptr);
        }
        removeLegacyFile(e);
        return nullptr;
    }
    if(std::chrono::steady_clock::now()<retryAt)return error.c_str();
    if(e->PushLocalFrame(96)!=JNI_OK)return "Minecraft is busy";
    try { reload(e,enabled,model);error.clear(); }
    catch(const std::exception& ex) {
        if(e->ExceptionCheck())e->ExceptionClear();
        error=ex.what();retryAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    }
    e->PopLocalFrame(nullptr);
    if(!error.empty())return error.c_str();
    return pending?"Loading cosmetic":nullptr;
}
bool restoreResources(JNIEnv* e) {
    stopping=true;
    try { removeRouter(e); } catch(...) {if(e->ExceptionCheck())e->ExceptionClear();return false;}
    return prepareResources(e,false,0)==nullptr;
}
void shutdownResources(JNIEnv* e) {
    if(!restoreResources(e)) { blockUnload();return; }
    if(e->PushLocalFrame(32)!=JNI_OK) { blockUnload();return; }
    try {
        if(provider) { auto mc=minecraft(e);providerSet(e,manager(e,mc),false); }
        if(!drainNativeCallbacks()) { e->PopLocalFrame(nullptr);blockUnload();return; }
        if(bridge)e->UnregisterNatives(bridge);
        for(jobject ref:{static_cast<jobject>(bridge),loader,provider,packInfo,itemManager,originalGetter,modelRouter})if(ref)e->DeleteGlobalRef(ref);
        bridge=nullptr;loader=provider=packInfo=nullptr;
        itemManager=originalGetter=modelRouter=nullptr;
    } catch(...) { if(e->ExceptionCheck())e->ExceptionClear();blockUnload(); }
    e->PopLocalFrame(nullptr);
}
}
