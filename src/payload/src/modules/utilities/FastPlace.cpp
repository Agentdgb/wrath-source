#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"

#include <string>

namespace dt::modules {

namespace {

bool isStackEligible(JNIEnv* env, jobject stack, bool blocksOnly) {
    if (!stack) return false;

    // Check isEmpty
    if (dt::jvm::callBoolCanon(env, stack, "ItemStack", "isEmpty")) {
        return false;
    }

    auto item = dt::jvm::callObjectCanon(env, stack, "ItemStack", "getItem");
    if (!item) return false;

    jmethodID getKeyMid = dt::jvm::methodCanon(env, "Item", "getTranslationKey");
    if (!getKeyMid) return false;

    jstring jKey = static_cast<jstring>(env->CallObjectMethod(item.get(), getKeyMid));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!jKey) return false;

    const char* utf = env->GetStringUTFChars(jKey, nullptr);
    if (!utf) { env->DeleteLocalRef(jKey); return false; }
    std::string key(utf);
    env->ReleaseStringUTFChars(jKey, utf);
    env->DeleteLocalRef(jKey);

    // Any block placement item
    if (key.starts_with("block.")) return true;

    if (blocksOnly) return false;

    // Fast-throw projectiles and consumables (exempt from place delays)
    if (key.find("experience_bottle") != std::string::npos ||
        key.find("ender_pearl") != std::string::npos ||
        key.find("snowball") != std::string::npos ||
        key.find("egg") != std::string::npos ||
        key.find("splash_potion") != std::string::npos ||
        key.find("lingering_potion") != std::string::npos ||
        key.find("firework_rocket") != std::string::npos) {
        return true;
    }

    return false;
}

bool canFastPlace(JNIEnv* env, jobject player, bool blocksOnly) {
    if (!player) return false;

    // Never reset cooldown when actively consuming/using an item (eating, drinking, drawing bow, shielding)
    if (dt::jvm::callBoolCanon(env, player, "LivingEntity", "isUsingItem")) {
        return false;
    }

    // Check main hand
    auto mainStack = dt::jvm::callObjectCanon(env, player, "LivingEntity", "getMainHandStack");
    if (isStackEligible(env, mainStack.get(), blocksOnly)) {
        return true;
    }

    // Check off-hand (method_6079 in Yarn)
    jclass livingCls = env->GetObjectClass(player);
    if (!livingCls) return false;
    jmethodID getOffHandMid = env->GetMethodID(livingCls, "method_6079", "()Lnet/minecraft/class_1799;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!getOffHandMid) {
        getOffHandMid = env->GetMethodID(livingCls, "getOffHandStack", "()Lnet/minecraft/class_1799;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    bool offEligible = false;
    if (getOffHandMid) {
        jobject offStack = env->CallObjectMethod(player, getOffHandMid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); offStack = nullptr; }
        if (offStack) {
            offEligible = isStackEligible(env, offStack, blocksOnly);
            env->DeleteLocalRef(offStack);
        }
    }
    env->DeleteLocalRef(livingCls);
    return offEligible;
}

} // namespace

class FastPlace : public Module {
public:
    FastPlace() : Module("QuickPlace",
                          "Place blocks and throw items with no delay",
                          Category::Utilities) {
        registerSetting(&delay_);
        registerSetting(&blocksOnly_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            if (!isEnabled()) return;
            dt::jvm::JniScope s;
            if (!s) return;
            JNIEnv* env = s.get();

            auto mc = dt::jvm::staticCallObjectCanon(env, "MinecraftClient", "getInstance");
            if (!mc) return;

            auto player = dt::jvm::LocalRef<jobject>{
                env, dt::jvm::getObjectFieldCanon(env, mc.get(), "MinecraftClient", "player")};
            if (!player) return;

            if (canFastPlace(env, player.get(), blocksOnly_.value())) {
                dt::jvm::setIntFieldCanon(env, mc.get(), "MinecraftClient", "itemUseCooldown",
                                             delay_.value());
            }
        });
    }

private:
    IntSetting  delay_{this, "Delay", 0, 0, 4};
    BoolSetting blocksOnly_{this, "Blocks Only", false};
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::FastPlace)
