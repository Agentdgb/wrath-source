#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <cmath>

namespace vanta::modules {

class FullBright : public Module {
public:
    FullBright() : Module("FullBright",
                          "Turns the brightness all the way up",
                          Category::Visuals) {
        registerSetting(&gamma_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            if (!isEnabled()) return;
            applyGamma(gamma_.value());
        });
    }

private:
    FloatSetting gamma_{this, "Gamma", 16.f, 1.f, 100.f};
    double       originalGamma_ = 1.0;
    bool         saved_         = false;
    float        lastApplied_   = -1.f;

    void onEnable() override {
        lastApplied_ = -1.f;
        applyGamma(gamma_.value());
    }

    void onDisable() override {
        if (!saved_) return;
        restoreGamma();
        saved_ = false;
        lastApplied_ = -1.f;
    }

    void applyGamma(float target) {
        if (std::fabs(lastApplied_ - target) < 0.01f) return;

        vanta::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();

        auto mc = vanta::jvm::staticCallObjectCanon(env, "MinecraftClient", "getInstance");
        if (!mc) return;

        auto options = vanta::jvm::LocalRef<jobject>{
            env, vanta::jvm::getObjectFieldCanon(env, mc.get(), "MinecraftClient", "options")};
        if (!options) return;

        jclass goClass = env->GetObjectClass(options.get());
        if (!goClass) return;

        // Modern 1.19+ SimpleOption<Double> (class_7172)
        jmethodID getGammaMid = env->GetMethodID(goClass, "method_42436", "()Lnet/minecraft/class_7172;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!getGammaMid) {
            getGammaMid = env->GetMethodID(goClass, "getGamma", "()Lnet/minecraft/class_7172;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        jobject gammaOption = nullptr;
        if (getGammaMid) {
            gammaOption = env->CallObjectMethod(options.get(), getGammaMid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); gammaOption = nullptr; }
        }

        if (!gammaOption) {
            jfieldID gammaFid = env->GetFieldID(goClass, "field_1826", "Lnet/minecraft/class_7172;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (gammaFid) {
                gammaOption = env->GetObjectField(options.get(), gammaFid);
                if (env->ExceptionCheck()) { env->ExceptionClear(); gammaOption = nullptr; }
            }
        }

        if (gammaOption) {
            jclass soClass = env->GetObjectClass(gammaOption);
            jmethodID getValueMid = env->GetMethodID(soClass, "method_41753", "()Ljava/lang/Object;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!getValueMid) {
                getValueMid = env->GetMethodID(soClass, "getValue", "()Ljava/lang/Object;");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            jmethodID setValueMid = env->GetMethodID(soClass, "method_41748", "(Ljava/lang/Object;)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!setValueMid) {
                setValueMid = env->GetMethodID(soClass, "setValue", "(Ljava/lang/Object;)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            jclass doubleClass = env->FindClass("java/lang/Double");
            jmethodID doubleValueMid = doubleClass ? env->GetMethodID(doubleClass, "doubleValue", "()D") : nullptr;
            jmethodID valueOfMid = doubleClass ? env->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;") : nullptr;

            if (getValueMid && setValueMid && doubleClass && doubleValueMid && valueOfMid) {
                if (!saved_) {
                    jobject curVal = env->CallObjectMethod(gammaOption, getValueMid);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); curVal = nullptr; }
                    if (curVal) {
                        originalGamma_ = env->CallDoubleMethod(curVal, doubleValueMid);
                        env->DeleteLocalRef(curVal);
                        saved_ = true;
                    }
                }

                jobject targetVal = env->CallStaticObjectMethod(doubleClass, valueOfMid, static_cast<jdouble>(target));
                if (targetVal) {
                    env->CallVoidMethod(gammaOption, setValueMid, targetVal);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->DeleteLocalRef(targetVal);
                    lastApplied_ = target;
                }
            }
            if (doubleClass) env->DeleteLocalRef(doubleClass);
            env->DeleteLocalRef(soClass);
            env->DeleteLocalRef(gammaOption);
        } else {
            // Legacy primitive float fallback
            jfieldID legacyGammaFid = env->GetFieldID(goClass, "gamma", "F");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (legacyGammaFid) {
                if (!saved_) {
                    originalGamma_ = static_cast<double>(env->GetFloatField(options.get(), legacyGammaFid));
                    saved_ = true;
                }
                env->SetFloatField(options.get(), legacyGammaFid, target);
                lastApplied_ = target;
            }
        }
        env->DeleteLocalRef(goClass);
    }

    void restoreGamma() {
        vanta::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();

        auto mc = vanta::jvm::staticCallObjectCanon(env, "MinecraftClient", "getInstance");
        if (!mc) return;

        auto options = vanta::jvm::LocalRef<jobject>{
            env, vanta::jvm::getObjectFieldCanon(env, mc.get(), "MinecraftClient", "options")};
        if (!options) return;

        jclass goClass = env->GetObjectClass(options.get());
        if (!goClass) return;

        jmethodID getGammaMid = env->GetMethodID(goClass, "method_42436", "()Lnet/minecraft/class_7172;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!getGammaMid) {
            getGammaMid = env->GetMethodID(goClass, "getGamma", "()Lnet/minecraft/class_7172;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        jobject gammaOption = nullptr;
        if (getGammaMid) {
            gammaOption = env->CallObjectMethod(options.get(), getGammaMid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); gammaOption = nullptr; }
        }

        if (!gammaOption) {
            jfieldID gammaFid = env->GetFieldID(goClass, "field_1826", "Lnet/minecraft/class_7172;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (gammaFid) {
                gammaOption = env->GetObjectField(options.get(), gammaFid);
                if (env->ExceptionCheck()) { env->ExceptionClear(); gammaOption = nullptr; }
            }
        }

        if (gammaOption) {
            jclass soClass = env->GetObjectClass(gammaOption);
            jmethodID setValueMid = env->GetMethodID(soClass, "method_41748", "(Ljava/lang/Object;)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!setValueMid) {
                setValueMid = env->GetMethodID(soClass, "setValue", "(Ljava/lang/Object;)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            jclass doubleClass = env->FindClass("java/lang/Double");
            jmethodID valueOfMid = doubleClass ? env->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;") : nullptr;

            if (setValueMid && doubleClass && valueOfMid) {
                jobject originalVal = env->CallStaticObjectMethod(doubleClass, valueOfMid, static_cast<jdouble>(originalGamma_));
                if (originalVal) {
                    env->CallVoidMethod(gammaOption, setValueMid, originalVal);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->DeleteLocalRef(originalVal);
                }
            }
            if (doubleClass) env->DeleteLocalRef(doubleClass);
            env->DeleteLocalRef(soClass);
            env->DeleteLocalRef(gammaOption);
        } else {
            jfieldID legacyGammaFid = env->GetFieldID(goClass, "gamma", "F");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (legacyGammaFid) {
                env->SetFloatField(options.get(), legacyGammaFid, static_cast<jfloat>(originalGamma_));
            }
        }
        env->DeleteLocalRef(goClass);
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::FullBright)
