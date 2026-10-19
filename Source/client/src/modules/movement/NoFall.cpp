#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/PacketAccess.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "hook/PacketEvents.h"

#include <cmath>
#include <algorithm>

namespace vanta::modules {

class NoFall : public Module {
public:
    NoFall() : Module("NoFall",
                      "Prevents or mitigates fall damage safely",
                      Category::Movement) {
        registerSetting(&mode_);
        registerSetting(&minFallDistance_);
        registerSetting(&groundProximity_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&) {
            if (!isEnabled()) {
                reset();
                return;
            }
            if (hook::packets::unavailable()) {
                setDescription("Unavailable: packet event hooks could not be installed on this runtime");
                setEnabled(false);
                return;
            }
            updateFallState();
        });

        // Intercept movement packets only when safe and necessary
        listen<events::PacketSendEvent>([this](events::PacketSendEvent& e) {
            if (!isEnabled() || !e.env || !e.packet) return;

            // If packet is already on-ground or not falling dangerously, do not touch
            if (vanta::jvm::packet::moveIsOnGround(e.env, e.packet)) {
                accumulatedFall_ = 0.f;
                shouldSpoofNow_ = false;
                return;
            }

            if (mode_.index() == 0 /* Smart */) {
                // In Smart mode, only spoof on the landing window:
                // 1) Fall distance has exceeded the damage threshold (> minFallDistance)
                // 2) Player is falling downwards (negative velocity)
                // 3) Proximity/impact condition is satisfied
                if (shouldSpoofNow_) {
                    vanta::jvm::packet::moveSetOnGround(e.env, e.packet, true);
                    shouldSpoofNow_ = false;
                    accumulatedFall_ = 0.f;
                }
            } else if (mode_.index() == 1 /* Packet */) {
                // Classic packet mode: only kicks in above min distance, never on tiny jumps
                if (accumulatedFall_ >= minFallDistance_.value()) {
                    vanta::jvm::packet::moveSetOnGround(e.env, e.packet, true);
                    accumulatedFall_ = 0.f;
                }
            }
        }, Priority::High);
    }

    void onEnable() override {
        reset();
    }

    void onDisable() override {
        reset();
    }

private:
    EnumSetting  mode_{this, "Mode", {"Smart", "Packet"}, 0};
    FloatSetting minFallDistance_{this, "Min Fall", 3.0f, 2.5f, 10.0f};
    BoolSetting  groundProximity_{this, "Ground Check", true};

    double lastY_            = 0.0;
    float  accumulatedFall_  = 0.f;
    bool   hasLastY_         = false;
    bool   shouldSpoofNow_   = false;

    void reset() {
        accumulatedFall_ = 0.f;
        hasLastY_ = false;
        shouldSpoofNow_ = false;
    }

    void updateFallState() {
        vanta::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();

        auto mc = vanta::jvm::staticCallObjectCanon(env, "MinecraftClient", "getInstance");
        if (!mc) return;

        auto player = vanta::jvm::LocalRef<jobject>{
            env, vanta::jvm::getObjectFieldCanon(env, mc.get(), "MinecraftClient", "player")};
        if (!player) {
            reset();
            return;
        }

        // Stand down if touching water (water negates fall damage legitimately)
        if (vanta::jvm::callBoolCanon(env, player.get(), "Entity", "isTouchingWater")) {
            reset();
            return;
        }

        // Check vanilla onGround
        bool onGround = vanta::jvm::callBoolCanon(env, player.get(), "Entity", "isOnGround");
        if (onGround) {
            reset();
            return;
        }

        // Get player vertical position
        jmethodID getYMid = vanta::jvm::methodCanon(env, "Entity", "getY");
        if (!getYMid) return;
        double currentY = env->CallDoubleMethod(player.get(), getYMid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

        if (!hasLastY_) {
            lastY_ = currentY;
            hasLastY_ = true;
            accumulatedFall_ = 0.f;
            return;
        }

        double deltaY = lastY_ - currentY;
        lastY_ = currentY;

        if (deltaY > 0.0) {
            // Player is descending
            accumulatedFall_ += static_cast<float>(deltaY);
        } else if (deltaY < -0.15) {
            // Player is ascending (jumped up or launched) -> reset counter
            accumulatedFall_ = 0.f;
        }

        // If accumulated fall is below the damage threshold (3 blocks default), never prepare spoofing
        if (accumulatedFall_ < minFallDistance_.value()) {
            shouldSpoofNow_ = false;
            return;
        }

        // In Smart mode, check if vertical velocity indicates descent towards ground
        if (groundProximity_.value()) {
            auto velObj = vanta::jvm::LocalRef<jobject>{
                env, vanta::jvm::getObjectFieldCanon(env, player.get(), "Entity", "velocity")};
            double velY = 0.0;
            if (velObj) {
                velY = vanta::jvm::getDoubleFieldCanon(env, velObj.get(), "Vec3d", "y");
            }

            if (velY < -0.3) {
                shouldSpoofNow_ = true;
            }
        } else {
            shouldSpoofNow_ = true;
        }
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::NoFall)
