#include "Engine.h"
#include "jvm/JvmContext.h"
namespace vanta::modules::aimassist::wind {
Engine::~Engine() {
    legacy.stop();
    jvm::JniScope scope;
    if (scope) {
        regular::onDisable(scope.get(), legacy.shared);
        drop_cache(scope.get());
    }
}
void Engine::pause() {
    if (legacy.running.load() || legacy.moduleEnabled.load()) {
        legacy.stop();
        std::lock_guard lock(legacy.control);
        jvm::JniScope scope;
        if (scope)
            regular::onDisable(scope.get(), legacy.shared);
    }
    state_ = "Paused";
}
void Engine::step(JNIEnv *env, const SettingsView &settings) {
    std::lock_guard lock(legacy.control);
    if (!aimassist::ensure_cache(env)) {
        state_ = "Legacy JNI unavailable";
        return;
    }
    const auto &cache = aimassist::cache();
    if (!cache.mc_instance_fid || !cache.mc_player_fid || !cache.ent_yaw_fid || !cache.ent_pitch_fid) {
        state_ = "Legacy JNI fields missing";
        return;
    }
    legacy.settings = settings;
    const auto &sv = legacy.settings;
    auto mc = env->GetStaticObjectField(cache.minecraft_class, cache.mc_instance_fid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        state_ = "Legacy client read failed";
        return;
    }
    if (!mc) {
        state_ = "No game";
        return;
    }
    auto player = env->GetObjectField(mc, cache.mc_player_fid);
    env->DeleteLocalRef(mc);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        state_ = "Legacy player read failed";
        return;
    }
    if (!player) {
        state_ = "No player";
        return;
    }
    legacy.start();
    legacy.moduleEnabled.store(true);
    aimassist::regular::renderApply(env, player, legacy.shared, sv);
    env->DeleteLocalRef(player);
    std::lock_guard sharedLock(legacy.shared.mx);
    state_ = legacy.shared.targetRef ? "Tracking" : "Searching";
}

} // namespace vanta::modules::aimassist::wind
