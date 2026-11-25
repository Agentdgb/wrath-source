#include "modules/misc/spectate/spectate.h"
#include "jvm/JvmContext.h"
#include <jni.h>
#include <jnihook.h>
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
namespace dt::jvm {
JvmContext state;
JvmContext& ctx() noexcept { return state; }
jclass findClassAnyLoader(JNIEnv* e,const char* name) { return e->FindClass(name); }
}
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    JavaVM* vm=nullptr; JNIEnv* e=nullptr; int result=0;
    try {
        auto library=LoadLibraryA(WRATH_TEST_JVM_DLL);
        require(library!=nullptr,"JVM unavailable");
        using Create=jint(JNICALL*)(JavaVM**,void**,void*);
        auto create=reinterpret_cast<Create>(GetProcAddress(library,"JNI_CreateJavaVM"));
        const std::string folder=argc>1&&std::string(argv[1])=="legacy"?WRATH_TEST_LEGACY_CLASSPATH:WRATH_TEST_CLASSPATH;
        std::string cp="-Djava.class.path="+folder;
        JavaVMOption options[]={{cp.data(),nullptr},{const_cast<char*>("-Xverify:all"),nullptr}};
        JavaVMInitArgs args{JNI_VERSION_1_8,2,options,JNI_FALSE};
        require(create&&create(&vm,reinterpret_cast<void**>(&e),&args)==JNI_OK,"Create JVM");
        dt::jvm::ctx().vm.store(vm);
        auto cls=e->FindClass("net/minecraft/SpectateFixture");
        require(cls!=nullptr,"Fixture missing");
        auto cameraId=e->GetStaticMethodID(cls,"cameraId","()I");
        auto camera=[&]{return e->CallStaticIntMethod(cls,cameraId);};
        auto call=[&](const char* method){e->CallStaticVoidMethod(cls,e->GetStaticMethodID(cls,method,"()V"));};
        namespace spec=dt::modules::spectate;
        spec::request("Alpha"); spec::tick(e);
        require(!e->ExceptionCheck()&&spec::resolved()&&camera()==2,"Profile accessor / first camera attach failed");
        spec::request("Beta"); spec::tick(e);
        require(spec::resolved()&&camera()==3,"Switch retained the previous pinned target");
        spec::borrow_camera(e); require(camera()==1,"Camera loan did not restore local player");
        spec::return_camera(e); require(camera()==3,"Camera loan did not return to target");
        spec::stop(); spec::tick(e); require(!spec::active()&&!spec::resolved()&&camera()==1,"Stop did not restore camera");
        spec::request("Beta"); spec::tick(e); call("removeBeta"); spec::tick(e);
        require(!spec::resolved()&&spec::active()&&camera()==1,"Despawn did not restore the local camera");
        call("addBeta"); spec::request("Beta"); spec::tick(e);
        require(spec::resolved()&&camera()==3,"Retry did not reacquire a loaded player");
        spec::request("Unknown"); spec::tick(e);
        require(!spec::resolved()&&camera()==1&&std::string(spec::status()).find("distance")!=std::string::npos,"Range failure was not reported");
        spec::request("Local"); spec::tick(e); require(!spec::active()&&camera()==1,"Self selection did not stop");
        call("allowServer");
        auto count=e->GetStaticMethodID(cls,"commandCount","()I");
        auto spectator=e->GetStaticMethodID(cls,"spectator","()Z");
        spec::request_server("Beta");spec::tick(e);spec::tick(e);
        require(spec::resolved()&&camera()==3&&e->CallStaticBooleanMethod(cls,spectator),"Server spectator mode did not attach");
        require(!spec::lying()&&spec::camera_entity_id()==3,"Server mode reused the local camera override");
        spec::tick(e);spec::tick(e);
        require(e->CallStaticIntMethod(cls,count)==2,"Server commands repeated every frame");
        spec::stop();spec::tick(e);
        require(camera()==1&&e->CallStaticBooleanMethod(cls,spectator),"Stop did not release into free spectator flight");
        spec::request_server("Alpha");spec::tick(e);
        require(spec::resolved()&&camera()==2,"Existing server spectator failed");
        call("releaseCamera");spec::tick(e);
        require(!spec::active()&&!spec::resolved(),"Vanilla exit was overridden by the client");
        call("denyServer");spec::request_server("Beta");spec::tick(e);spec::tick(e);spec::tick(e);
        require(!spec::resolved()&&camera()==1&&!e->CallStaticBooleanMethod(cls,spectator),"Client pretended a refused gamemode succeeded");
        require(e->CallStaticIntMethod(cls,count)==1,"Refused gamemode command spammed the server");
        spec::stop();spec::tick(e);const int previous=e->CallStaticIntMethod(cls,count);
        spec::request_server("Beta; op me");spec::tick(e);
        require(e->CallStaticIntMethod(cls,count)==previous&&!spec::resolved(),"Invalid account name reached the command channel");
        spec::stop();spec::tick(e);
        call("disconnect"); spec::request("Beta"); spec::tick(e);
        require(!spec::resolved()&&std::string(spec::status()).find("world")!=std::string::npos,"Disconnect was not reported");
        spec::shutdown(e);
        require(!e->ExceptionCheck(),"Spectate left a pending exception");
        std::cout<<"Spectate passed: profile accessors, attach/switch, loan/return, stop, despawn/retry, range and disconnect.\n";
    } catch(const std::exception& ex) { std::cerr<<ex.what()<<'\n'; if(e&&e->ExceptionCheck()){e->ExceptionDescribe();e->ExceptionClear();} result=1; }
    JNIHook_Shutdown(); if(vm) vm->DestroyJavaVM(); return result;
}
