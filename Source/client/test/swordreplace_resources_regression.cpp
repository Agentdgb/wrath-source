#include "modules/render/swordreplace/SwordReplaceResources.h"
#include "modules/render/swordreplace/SwordReplace.h"
#include "core/UnloadSafety.h"
#include "jvm/JniHelper.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
namespace Hook { void ensure_tls_for_this_thread() {} }
namespace vanta::jvm {
jclass findClassAnyLoader(JNIEnv* env,const char* name) {auto c=env->FindClass(name);if(env->ExceptionCheck())env->ExceptionClear();return c;}
}
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    using namespace vanta::modules::swordreplace;
    std::filesystem::path path(WRATH_TEST_JVM_DLL);
    SetDllDirectoryW(path.parent_path().parent_path().c_str());
    auto library=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    require(library!=nullptr,"cannot load JVM");
    using CreateVm=jint(JNICALL*)(JavaVM**,void**,void*);
    auto create=reinterpret_cast<CreateVm>(GetProcAddress(library,"JNI_CreateJavaVM"));
    std::string cp=std::string("-Djava.class.path=")+WRATH_TEST_CLASSPATH;
    JavaVMOption option{cp.data(),nullptr};JavaVMInitArgs args{JNI_VERSION_1_8,1,&option,JNI_FALSE};
    JavaVM* vm=nullptr;JNIEnv* env=nullptr;
    require(create && create(&vm,reinterpret_cast<void**>(&env),&args)==JNI_OK,"JVM creation failed");
    int result=0;
    try {
        auto cls=env->FindClass("net/minecraft/class_310");
        auto instance=env->GetStaticMethodID(cls,"method_1551","()Lnet/minecraft/class_310;");
        auto mc=env->CallStaticObjectMethod(cls,instance);
        auto complete=env->GetStaticMethodID(cls,"completeReload","()V");
        auto json=env->GetStaticMethodID(cls,"cosmeticJson","(I)Ljava/lang/String;");
        auto renderSword=env->GetStaticMethodID(cls,"renderSword","()Ljava/lang/String;");
        auto renderOther=env->GetStaticMethodID(cls,"renderOther","()Ljava/lang/String;");
        auto reloads=env->GetFieldID(cls,"reloads","I");
        auto render=[&](jmethodID method) {
            auto value=static_cast<jstring>(env->CallStaticObjectMethod(cls,method));
            require(value && !env->ExceptionCheck(),"model lookup raised a Java exception");
            const char* text=env->GetStringUTFChars(value,nullptr);std::string result(text);
            env->ReleaseStringUTFChars(value,text);env->DeleteLocalRef(value);return result;
        };
        auto count=env->GetStaticMethodID(cls,"countResources","()I");
        auto read=env->GetStaticMethodID(cls,"readResource","(Ljava/lang/String;)[B");
        auto getNames=[&]() {
            auto manager=env->GetObjectField(mc,env->GetFieldID(cls,"field_1715","Lnet/minecraft/class_3283;"));
            auto pc=env->GetObjectClass(manager);
            return env->CallObjectMethod(manager,env->GetMethodID(pc,"method_29210","()Ljava/util/Collection;"));
        };
        auto collection=env->FindClass("java/util/Collection");
        auto contains=env->GetMethodID(collection,"contains","(Ljava/lang/Object;)Z");
        for(int i=0;i<20;++i) {
            const char* message=prepareResources(env,true,i);
            if(i==0)require(message && std::string_view(message)=="Loading cosmetic",message?message:"initial preload not started");
            else require(message==nullptr,"changing a cosmetic reloaded resources");
            require(!env->ExceptionCheck(),"native provider raised a Java exception");
            auto value=static_cast<jstring>(env->CallStaticObjectMethod(cls,json,i));
            require(value && !env->ExceptionCheck(),"native in-memory model not available");
            const char* chars=env->GetStringUTFChars(value,nullptr);
            require(std::string_view(chars).find(modelPaths[i])!=std::string_view::npos,"wrong cosmetic rendered");
            require(std::string_view(chars).find("custom_model_data")==std::string_view::npos,"cosmetic still needs render hook");
            env->ReleaseStringUTFChars(value,chars);env->DeleteLocalRef(value);
            require(env->CallStaticIntMethod(cls,count)>=20,"memory geometry discovery failed");
            if(i==0) for(unsigned n=0;n<assetCount;++n) {
                std::string_view path=assets[n].path;
                if(!path.starts_with("assets/minecraft/"))continue;
                path.remove_prefix(17);
                auto location=env->NewStringUTF(std::string(path).c_str());
                auto data=static_cast<jbyteArray>(env->CallStaticObjectMethod(cls,read,location));
                require(data && !env->ExceptionCheck() && env->GetArrayLength(data)==static_cast<jsize>(assets[n].size),"embedded mesh/texture not delivered to JVM");
                auto bytes=env->GetByteArrayElements(data,nullptr);
                require(std::memcmp(bytes,assets[n].bytes,assets[n].size)==0,"JVM received altered cosmetic data");
                env->ReleaseByteArrayElements(data,bytes,JNI_ABORT);
                env->DeleteLocalRef(data);env->DeleteLocalRef(location);
            }
            const auto* preview=findAsset("preview/"+std::to_string(i)+".png");
            require(preview && preview->size>100 && preview->bytes[0]==137,"cosmetic preview missing");
            if(i==0)env->CallStaticVoidMethod(cls,complete);
            require(!prepareResources(env,true,i),"finished cosmetic reload not accepted");
            require(env->GetIntField(mc,reloads)==1,"selection repeated the loading screen");
            require(render(renderSword)=="baked:minecraft:wrath_cosmetic/"+std::to_string(i),"preloaded sword lookup selected a wrong model");
            require(render(renderOther)=="baked:minecraft:apple","cosmetic changed a non-sword item");
            auto names=getNames();
            for(const char* preserved:{"vanilla","file/existing.zip","wrath:cosmetics"}) {
                auto name=env->NewStringUTF(preserved);require(env->CallBooleanMethod(names,contains,name),"lost another resource source");env->DeleteLocalRef(name);
            }
        }
        require(!prepareResources(env,false,0),"disabling cosmetic reloaded resources");
        require(render(renderSword)=="baked:minecraft:diamond_sword" && env->GetIntField(mc,reloads)==1,"disable did not restore sword immediately");
        require(!prepareResources(env,true,0) && render(renderSword)=="baked:minecraft:wrath_cosmetic/0","reenabling did not use preloaded sword");
        require(!restoreResources(env),"shutdown ignored pending visual restoration");
        env->CallStaticVoidMethod(cls,complete);
        require(restoreResources(env),"native cosmetic restoration failed");
        auto names=getNames();auto name=env->NewStringUTF("wrath:cosmetics");
        require(!env->CallBooleanMethod(names,contains,name),"cosmetic still equipped after disable");
        name=env->NewStringUTF("file/wrath-swordreplace-godweer.zip");
        require(!env->CallBooleanMethod(names,contains,name),"legacy external pack still active");
        shutdownResources(env);require(vanta::safeToUnload(),"native cosmetic cleanup blocked unload");
        std::cout<<"Native DLL/JVM provider: 20 direct cosmetics, embedded meshes/textures, previews, preservation, disable and cleanup passed without JVMTI hooks or disk packs\n";
    } catch(const std::exception& ex) {
        if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}
        std::cerr<<ex.what()<<'\n';result=1;
    }
    vm->DestroyJavaVM();return result;
}
