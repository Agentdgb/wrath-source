#include "modules/combat/shieldbreaker/ShieldBreaker.h"
#include "modules/combat/shieldbreaker/ShieldBreakerLogic.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <limits>

namespace {
dt::modules::triggerbot::jni_cache cache;
bool friend_target = false;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
namespace dt::modules::triggerbot {
const jni_cache& get_cache() { return cache; }
}
namespace dt::social {
bool isFriend(JNIEnv*, jobject) { return friend_target; }
}

int main() {
    using namespace dt::modules::shieldbreaker;
    const std::filesystem::path runtimePath(WRATH_TEST_JVM_DLL);
    SetDllDirectoryW(runtimePath.parent_path().parent_path().c_str());
    auto runtime = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!runtime) return 1;
    using Create = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<Create>(GetProcAddress(runtime, "JNI_CreateJavaVM"));
    std::string classpath = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
    JavaVMOption option{classpath.data(), nullptr};
    JavaVMInitArgs args{JNI_VERSION_1_8, 1, &option, JNI_FALSE};
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create || create(&vm, reinterpret_cast<void**>(&env), &args) != JNI_OK) return 1;
    int result = 0;
    try {
        auto mcClass = env->FindClass("ShieldFixture");
        auto playerClass = env->FindClass("ShieldFixture$Player");
        auto invClass = env->FindClass("ShieldFixture$Inventory");
        auto hitClass = env->FindClass("ShieldFixture$Hit");
        auto stackClass = env->FindClass("ShieldFixture$Stack");
        auto itemClass = env->FindClass("ShieldFixture$Item");
        require(mcClass && playerClass && invClass && hitClass && stackClass && itemClass, "fixture classes missing");
        cache.player_class = playerClass;
        cache.entity_hit_result_class = hitClass;
        cache.minecraft_player_fid = env->GetFieldID(mcClass,"player","LShieldFixture$Player;");
        cache.minecraft_current_screen_fid = env->GetFieldID(mcClass,"screen","Ljava/lang/Object;");
        cache.minecraft_crosshair_target_fid = env->GetFieldID(mcClass,"hit","LShieldFixture$Hit;");
        cache.player_inventory_fid = env->GetFieldID(playerClass,"inventory","LShieldFixture$Inventory;");
        cache.inventory_selected_slot_fid = env->GetFieldID(invClass,"selected","I");
        cache.inventory_get_stack_mid = env->GetMethodID(invClass,"stack","(I)LShieldFixture$Stack;");
        cache.itemstack_is_empty_mid = env->GetMethodID(stackClass,"empty","()Z");
        cache.itemstack_get_item_mid = env->GetMethodID(stackClass,"item","()LShieldFixture$Item;");
        cache.item_get_translation_key_mid = env->GetMethodID(itemClass,"key","()Ljava/lang/String;");
        cache.entity_hit_result_get_entity_mid = env->GetMethodID(hitClass,"target","()LShieldFixture$Player;");
        cache.living_entity_is_blocking_mid = env->GetMethodID(playerClass,"blocking","()Z");
        cache.living_entity_is_using_item_mid = env->GetMethodID(playerClass,"using","()Z");
        cache.living_entity_get_health_mid = env->GetMethodID(playerClass,"health","()F");
        cache.entity_yaw_fid = env->GetFieldID(playerClass,"yaw","F");
        cache.entity_get_x_mid = env->GetMethodID(playerClass,"x","()D");
        cache.entity_get_z_mid = env->GetMethodID(playerClass,"z","()D");
        require(!env->ExceptionCheck(), "fixture mapping failed");
        auto mc = env->NewObject(mcClass, env->GetMethodID(mcClass,"<init>","()V"));
        auto player = env->GetObjectField(mc,cache.minecraft_player_fid);
        auto inventory = env->GetObjectField(player,cache.player_inventory_fid);
        auto hit = env->GetObjectField(mc,cache.minecraft_crosshair_target_fid);
        auto target = env->CallObjectMethod(hit,cache.entity_hit_result_get_entity_mid);
        auto selected = [&] { return env->GetIntField(inventory,cache.inventory_selected_slot_fid); };
        auto slot = [&](int value) { env->SetIntField(inventory,cache.inventory_selected_slot_fid,value); };
        require(!can_break(env,mc,target), "disabled module accepted target");
        { AttackSlot attack(env,mc); require(selected()==0,"disabled module swapped"); }
        set_enabled(true);
        require(can_break(env,mc,target) && selected()==0,"query modified inventory");
        { AttackSlot attack(env,mc); require(selected()==5,"did not choose netherite axe"); }
        require(selected()==0,"attack/cancellation did not restore sword");
        { AttackSlot attack(env,mc); slot(8); }
        require(selected()==8,"manual slot change was overwritten");
        { AttackSlot attack(env,mc); require(selected()==8,"food was replaced"); }
        slot(5);
        { AttackSlot attack(env,mc); require(selected()==5,"held axe changed"); }
        require(selected()==5,"held axe restored incorrectly");
        slot(0);
        friend_target = true;
        require(!can_break(env,mc,target),"friend accepted");
        friend_target = false;
        auto z = env->GetFieldID(playerClass,"z","D");
        env->SetDoubleField(player,z,-2);
        require(!can_break(env,mc,target),"target facing away triggered shield breaker");
        env->SetDoubleField(player,z,2);
        env->SetObjectField(mc,cache.minecraft_current_screen_fid,mc);
        { AttackSlot attack(env,mc); require(selected()==0,"menu triggered swap"); }
        env->SetObjectField(mc,cache.minecraft_current_screen_fid,nullptr);
        auto blocking = env->GetFieldID(playerClass,"blocking","Z");
        env->SetBooleanField(target,blocking,JNI_FALSE);
        require(!can_break(env,mc,target),"lowered shield accepted");
        env->SetBooleanField(target,blocking,JNI_TRUE);
        auto health = env->GetFieldID(playerClass,"health","F");
        env->SetFloatField(target,health,0);
        require(!can_break(env,mc,target),"dead target accepted");
        env->SetFloatField(target,health,20);
        auto usingItem = env->GetFieldID(playerClass,"using","Z");
        env->SetBooleanField(player,usingItem,JNI_TRUE);
        require(!can_break(env,mc,target),"item use interrupted");
        env->SetBooleanField(player,usingItem,JNI_FALSE);
        auto broken = env->GetFieldID(invClass,"broken","Z");
        env->SetBooleanField(inventory,broken,JNI_TRUE);
        require(!can_break(env,mc,target) && !env->ExceptionCheck(),"inventory exception escaped");
        env->SetBooleanField(inventory,broken,JNI_FALSE);
        {
            AttackSlot attack(env,mc);
            env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),"attack failed");
        }
        require(env->ExceptionCheck(),"original attack exception lost");
        env->ExceptionClear();
        require(selected()==0,"failed attack did not restore");
        auto saved = cache.inventory_get_stack_mid;
        cache.inventory_get_stack_mid = nullptr;
        require(!can_break(env,mc,target),"missing mapping accepted");
        cache.inventory_get_stack_mid = saved;
        require(axe_rank("item.minecraft.netherite_pickaxe")<0,"pickaxe classified as axe");
        require(axe_rank("item.minecraft.copper_axe")>=0,"copper axe missing");
        require(!should_restore(4,5,0),"unowned slot would be restored");
        require(shield_faces_attacker(0,0,2),"front shield rejected");
        require(shield_faces_attacker(90,-2,0),"rotated front shield rejected");
        require(!shield_faces_attacker(0,0,-2),"rear shield accepted");
        require(!shield_faces_attacker(0,0,0),"overlapping positions accepted");
        require(!shield_faces_attacker(0,std::numeric_limits<double>::quiet_NaN(),2),"NaN position accepted");
        configure({true,true,true,false,150});
        { AttackSlot attack(env,mc); require(selected()==3,"first-axe setting ignored"); attack.complete(); }
        require(selected()==3 && has_pending_return(),"axe was not left visibly selected");
        pump_return(env,mc,50);
        pump_return(env,mc,50);
        require(selected()==3,"return delay ended too early");
        pump_return(env,mc,50);
        require(selected()==0 && !has_pending_return(),"delayed return failed");
        configure({true,true,true,true,150});
        { AttackSlot attack(env,mc); attack.complete(); }
        slot(8);
        pump_return(env,mc,200);
        require(selected()==8 && !has_pending_return(),"delayed return overwrote manual slot change");
        configure({false,false,true,true,150});
        { AttackSlot attack(env,mc); require(selected()==5,"OnlySwords off ignored"); attack.complete(); }
        require(selected()==5 && !has_pending_return(),"SwitchBack off ignored");
        slot(0);
        configure({true,true,false,true,150});
        env->SetDoubleField(player,z,-2);
        require(can_break(env,mc,target),"CheckFacing off ignored");
        env->SetDoubleField(player,z,2);
        configure({});
        { AttackSlot attack(env,mc); attack.complete(); }
        set_enabled(false);
        pump_return(env,mc,1);
        require(selected()==0 && !has_pending_return(),"disable left pending slot change");
        set_enabled(true);
        { AttackSlot attack(env,mc); attack.complete(); }
        clear_pending_return(env,true);
        require(selected()==0 && !has_pending_return(),"unload cleanup failed");
        { AttackSlot attack(env,mc); attack.complete(); }
        pump_return(env,nullptr,10);
        require(!has_pending_return(),"disconnect kept inventory reference");
        slot(0);
        env->CallVoidMethod(inventory,env->GetMethodID(invClass,"clearAxes","()V"));
        require(!can_break(env,mc,target),"no axe available but shield accepted");
        { AttackSlot attack(env,mc); require(selected()==0,"missing axe changed slot"); }
        require(!env->ExceptionCheck(),"pending JNI exception");
        std::cout << "ShieldBreaker: JVM selection, cancellation, restoration, manual changes and failure cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        result = 1;
    }
    set_enabled(false);
    clear_pending_return(env,false);
    vm->DestroyJavaVM();
    return result;
}
