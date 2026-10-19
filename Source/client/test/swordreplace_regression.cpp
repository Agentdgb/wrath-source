#include "../src/modules/render/swordreplace/SwordReplace.cpp"
#include <iostream>
#include <stdexcept>
#include <set>
namespace { bool booted=false, runtime=false, loading=false, lastEnabled=false; int calls=0,lastModel=-1; }
namespace vanta::entry { bool bootstrapFinished() noexcept { return booted; } }
namespace vanta::jvm {
JvmContext context;
JvmContext& ctx() noexcept { return context; }
JniScope::JniScope() { if(runtime)env_=reinterpret_cast<JNIEnv*>(1); }
JniScope::~JniScope() = default;
}
namespace vanta::modules::swordreplace {
const char* prepareResources(JNIEnv*,bool enabled,int model) { ++calls;lastEnabled=enabled;lastModel=model;return loading?"Loading cosmetic":nullptr; }
bool restoreResources(JNIEnv*) { return !loading; }
void shutdownResources(JNIEnv*) {}
}
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
int main() {
    using namespace vanta::modules::swordreplace;
    try {
        std::set<std::string_view> unique(std::begin(models),std::end(models));
        require(unique.size()==20 && std::string_view(models[defaultModel])=="Katana","cosmetic catalogue/default incorrect");
        update(true,0); require(calls==0,"cosmetic touched JVM before bootstrap");
        booted=true;vanta::jvm::ctx().vm.store(reinterpret_cast<JavaVM*>(1));runtime=true;
        update(true,3);require(lastEnabled && lastModel==3 && std::string_view(status())=="Cosmetic equipped","activation failed");
        loading=true;update(true,7);require(std::string_view(status())=="Loading cosmetic","pending status lost");
        require(!prepareShutdown(),"shutdown freed pending cosmetic resources");
        loading=false;update(true,99);require(lastModel==19,"invalid saved index not clamped");
        update(false,0);require(!lastEnabled && std::string_view(status())=="Sword cosmetics - 20 models","disable not forwarded to JVM");
        require(prepareShutdown(),"finished cosmetic could not shut down");
        std::cout<<"20 cosmetics, deferred native JVM activation, selection, disable and pending shutdown passed\n";
        return 0;
    } catch(const std::exception& ex) { std::cerr<<ex.what()<<'\n';return 1; }
}
