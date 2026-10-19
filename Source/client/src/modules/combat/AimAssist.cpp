

#include "aimassist/aimassist.h"
#include "aimassist/aimassist_debug.h"

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/social/EntityFriends.h"   // a friend is not a target
#include "jvm/JniRef.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

namespace vanta::modules::aimassist {

namespace {
Runtime g_rt;

void workerLoop(Runtime* rt)
{
#pragma comment(lib, "winmm.lib")
    const UINT timerPeriod = 1;
    bool timerActive = false;

    auto setTimerHighRes = [&](bool want) {
        if (want && !timerActive) {
            if (timeBeginPeriod(timerPeriod) == TIMERR_NOERROR)
                timerActive = true;
        } else if (!want && timerActive) {
            timeEndPeriod(timerPeriod);
            timerActive = false;
        }
    };

    JavaVM* vm = nullptr;
    while (rt->running.load(std::memory_order_acquire) && !vm) {
        vm = vanta::jvm::ctx().vm.load();
        if (!vm) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    if (!vm) return;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK
        || !env) {
        return;
    }

    int blockBreakCooldown = 0;

    long long cadenceStart = 0;
    int cadenceTicks = 0;
    bool cadenceLogged = false;

    SettingsView cachedSv{};
    while (rt->running.load(std::memory_order_acquire)) {
        if (!rt->moduleEnabled.load(std::memory_order_acquire)) {
            setTimerHighRes(false);
            debug::workerStage("module disabled");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        if (!timerActive) {
            setTimerHighRes(true);
            if (!cadenceLogged && cadenceTicks == 0) {
                cadenceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
        }

        debug::workerStage("entering worker tick");
        {
            std::unique_lock lk(rt->settingsMx, std::try_to_lock);
            if (lk.owns_lock()) {
                cachedSv = rt->settings;
            }
        }
        const SettingsView sv = cachedSv;

        if (sv.mode == 1) lockon::workerTick(env, rt->shared, sv, blockBreakCooldown);
        else              regular::workerTick(env, rt->shared, sv, blockBreakCooldown);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (debug::active()) {
            static auto lastTick = std::chrono::steady_clock::now();
            const auto nowTick = std::chrono::steady_clock::now();
            aimassist::debug::workerTick(
                std::chrono::duration<double, std::milli>(nowTick - lastTick).count());
            lastTick = nowTick;
            aimassist::debug::optsTick();
        }

        if (!cadenceLogged) {
            ++cadenceTicks;
            if (cadenceTicks >= 2000) {
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()
                    - cadenceStart;

                char rate[32];
                std::snprintf(rate, sizeof(rate), "%.3f",
                              ms > 0 ? static_cast<double>(ms) / cadenceTicks : 0.0);
                VLOG_INFO("aimassist", "worker cadence: {} ticks / {} ms = {} ms per tick "
                          "(timer {})",
                          cadenceTicks, ms, rate,
                          timerActive ? "1ms OK" : "FAILED");
                cadenceLogged = true;
            }
        }
    }
    setTimerHighRes(false);
    vm->DetachCurrentThread();
}
}

void Runtime::start()
{
    if (running.exchange(true, std::memory_order_acq_rel)) return;
    worker = std::thread(workerLoop, this);
}

void Runtime::stop()
{
    running.store(false, std::memory_order_release);
    moduleEnabled.store(false, std::memory_order_release);
    if (worker.joinable()) worker.join();
}

namespace {

std::string heldItemKey(JNIEnv* env, jobject player)
{
    const JniCache& c = cache();
    if (!player || !c.living_mainhand_mid || !c.stack_get_item_mid || !c.item_get_key_mid)
        return {};
    jobject stack = env->CallObjectMethod(player, c.living_mainhand_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (!stack) return {};
    jobject item = env->CallObjectMethod(stack, c.stack_get_item_mid);
    env->DeleteLocalRef(stack);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (!item) return {};
    jstring key = static_cast<jstring>(env->CallObjectMethod(item, c.item_get_key_mid));
    env->DeleteLocalRef(item);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (!key) return {};
    const char* k = env->GetStringUTFChars(key, nullptr);
    std::string out = k ? k : "";
    if (k) env->ReleaseStringUTFChars(key, k);
    env->DeleteLocalRef(key);
    return out;
}

float weaponScore(const std::string& key)
{
    if (key.find("sword")   != std::string::npos) return 8.f;
    if (key.find("mace")    != std::string::npos) return 7.5f;
    if (key.find("trident") != std::string::npos) return 7.f;
    if (key.find("axe")     != std::string::npos) return 6.5f;
    return 1.f;
}

float threatScore(JNIEnv* env, jobject target)
{
    return weaponScore(heldItemKey(env, target));
}

float equipmentScore(JNIEnv* env, jobject target)
{
    const JniCache& c = cache();
    if (!c.player_class || env->IsInstanceOf(target, c.player_class) != JNI_TRUE)
        return 0.f;
    float score = 0.f;
    if (c.living_armor_mid) {
        const jint armor = env->CallIntMethod(target, c.living_armor_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else score += static_cast<float>(armor);
    }
    if (c.player_inventory_fid && c.inv_get_stack_mid) {
        jobject inv = env->GetObjectField(target, c.player_inventory_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); inv = nullptr; }
        if (inv) {
            for (int i = 0; i < 9; ++i) {
                jobject st = env->CallObjectMethod(inv, c.inv_get_stack_mid, i);
                if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
                if (!st) continue;
                jobject item = (c.stack_get_item_mid && c.item_get_key_mid)
                    ? env->CallObjectMethod(st, c.stack_get_item_mid) : nullptr;
                env->DeleteLocalRef(st);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                if (!item) continue;
                jstring key = static_cast<jstring>(env->CallObjectMethod(item, c.item_get_key_mid));
                env->DeleteLocalRef(item);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                if (!key) continue;
                const char* k = env->GetStringUTFChars(key, nullptr);
                if (k) { score += weaponScore(k); env->ReleaseStringUTFChars(key, k); }
                env->DeleteLocalRef(key);
            }
            env->DeleteLocalRef(inv);
        }
    }
    return score;
}

bool isPlayerEntity(JNIEnv* env, jobject ent)
{
    return cache().player_class && env->IsInstanceOf(ent, cache().player_class) == JNI_TRUE;
}

}

double distanceToEntity(JNIEnv* env, jobject player, jobject target)
{
    const EntPos a = entityPos(env, player);
    const EntPos b = entityPos(env, target);
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool isDeadOrDying(JNIEnv* env, jobject target)
{
    const JniCache& c = cache();
    if (!target) return true;
    if (c.ent_is_alive_mid
        && env->CallBooleanMethod(target, c.ent_is_alive_mid) != JNI_TRUE)
        return true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (c.living_health_mid) {
        const float hp = env->CallFloatMethod(target, c.living_health_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (hp <= 0.f) return true;
    }
    return false;
}

namespace {

bool isValidTargetForPlayer(JNIEnv* env, jobject player, const EntPos& pp, float playerYaw,
                            jobject target, const SettingsView& sv,
                            EntPos& outTargetPos, double& outDist, int& outAngle)
{
    const JniCache& c = cache();
    if (!target || !player) return false;
    if (env->IsSameObject(target, player) == JNI_TRUE) return false;

    if (c.living_class && env->IsInstanceOf(target, c.living_class) != JNI_TRUE) return false;
    if (c.armor_stand_class && env->IsInstanceOf(target, c.armor_stand_class) == JNI_TRUE) return false;

    const bool isPlayer = isPlayerEntity(env, target);
    if (isPlayer && !sv.players) return false;
    if (!isPlayer && !sv.mobs) return false;

    if (isDeadOrDying(env, target)) return false;

    outTargetPos = entityPos(env, target);
    const double dx = outTargetPos.x - pp.x;
    const double dy = outTargetPos.y - pp.y;
    const double dz = outTargetPos.z - pp.z;
    outDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (outDist >= static_cast<double>(static_cast<int>(sv.distance))) return false;

    outAngle = angleToTarget(playerYaw, pp.x, pp.z, outTargetPos.x, outTargetPos.z);
    if (outAngle > static_cast<int>(sv.maxAngle) / 2) return false;

    if (isPlayer && ::vanta::social::isFriend(env, target)) return false;
    if (sv.ignoreInvisible && c.ent_invisible_mid
        && env->CallBooleanMethod(target, c.ent_invisible_mid) == JNI_TRUE)
        return false;

    return true;
}

} // namespace

bool isValidTarget(JNIEnv* env, jobject target, const SettingsView& sv)
{
    const JniCache& c = cache();
    if (!target) return false;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject player = mc && c.mc_player_fid ? env->GetObjectField(mc, c.mc_player_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); player = nullptr; }
    if (!player) { if (mc) env->DeleteLocalRef(mc); return false; }

    bool ok = true;
    if (sv.limitToItems) {
        const std::string key = heldItemKey(env, player);
        if (key.find("sword") == std::string::npos) ok = false;
    }

    if (ok) {
        const EntPos pp = entityPos(env, player);
        const float yaw = env->GetFloatField(player, c.ent_yaw_fid);
        EntPos tp{};
        double dist = 0.0;
        int ang = 0;
        ok = isValidTargetForPlayer(env, player, pp, yaw, target, sv, tp, dist, ang);
    }

    env->DeleteLocalRef(player);
    env->DeleteLocalRef(mc);
    return ok;
}

jobject findBestTarget(JNIEnv* env, const SettingsView& sv)
{
    const JniCache& c = cache();
    if (!c.mc_instance_fid || !c.mc_world_fid || !c.world_entities_mid
        || !c.mc_player_fid)
        return nullptr;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!mc) return nullptr;
    jobject player = env->GetObjectField(mc, c.mc_player_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); player = nullptr; }
    jobject world = env->GetObjectField(mc, c.mc_world_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); world = nullptr; }
    if (!player || !world) {
        if (player) env->DeleteLocalRef(player);
        if (world) env->DeleteLocalRef(world);
        env->DeleteLocalRef(mc);
        return nullptr;
    }

    if (sv.limitToItems) {
        const std::string key = heldItemKey(env, player);
        if (key.find("sword") == std::string::npos) {
            env->DeleteLocalRef(player);
            env->DeleteLocalRef(world);
            env->DeleteLocalRef(mc);
            return nullptr;
        }
    }

    jobject entities = env->CallObjectMethod(world, c.world_entities_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); entities = nullptr; }
    if (!entities) {
        env->DeleteLocalRef(player); env->DeleteLocalRef(world); env->DeleteLocalRef(mc);
        return nullptr;
    }
    jobject it = env->CallObjectMethod(entities, c.iterable_iterator_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); it = nullptr; }

    const EntPos pp = entityPos(env, player);
    const float playerYaw = env->GetFloatField(player, c.ent_yaw_fid);

    jobject best = nullptr;
    double bestKey = 0.0;
    bool haveBest = false;

    while (it && env->CallBooleanMethod(it, c.iterator_has_next_mid) == JNI_TRUE) {
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject ent = env->CallObjectMethod(it, c.iterator_next_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (!ent) continue;

        EntPos tp{};
        double dist = 0.0;
        int ang = 0;
        if (!isValidTargetForPlayer(env, player, pp, playerYaw, ent, sv, tp, dist, ang)) {
            env->DeleteLocalRef(ent);
            continue;
        }

        double key = 0.0;
        switch (sv.targetMode) {
            case 1:   key = dist; break;
            case 2:   {
                if (isPlayerEntity(env, ent))
                    key = equipmentScore(env, ent);
                else
                    key = dist;
                break;
            }
            case 3:   {
                if (isPlayerEntity(env, ent))
                    key = threatScore(env, ent);
                else
                    key = dist;
                break;
            }
            case 4:   {
                float hp = 0.f;
                if (c.living_health_mid)
                    hp = env->CallFloatMethod(ent, c.living_health_mid);
                if (env->ExceptionCheck()) env->ExceptionClear();
                key = hp;
                break;
            }
            default:  {
                key = ang;
                break;
            }
        }

        if (!haveBest || key < bestKey) {
            if (best) env->DeleteLocalRef(best);
            best = ent;
            bestKey = key;
            haveBest = true;
        } else {
            env->DeleteLocalRef(ent);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (it) env->DeleteLocalRef(it);
    env->DeleteLocalRef(entities);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(player);
    env->DeleteLocalRef(mc);
    return best;
}

bool canAim(JNIEnv* env, const SettingsView& sv, int& blockBreakCooldown)
{
    debug::workerStage("checking aim conditions");
    const JniCache& c = cache();
    if (!c.mc_instance_fid || !c.mc_player_fid) {
        debug::workerStage("missing Minecraft/player JNI fields");
        return false;
    }

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    jobject player = mc ? env->GetObjectField(mc, c.mc_player_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); player = nullptr; }
    if (!mc || !player) {
        if (player) env->DeleteLocalRef(player);
        if (mc) env->DeleteLocalRef(mc);
        return false;
    }

    bool ok = true;

    if (sv.checkBlockBreak && c.mc_crosshair_fid && c.block_hit_class) {
        jobject hit = env->GetObjectField(mc, c.mc_crosshair_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); hit = nullptr; }
        const bool aimingAtBlock = hit
            && env->IsInstanceOf(hit, c.block_hit_class) == JNI_TRUE;
        if (hit) env->DeleteLocalRef(hit);
        if (aimingAtBlock) {
            blockBreakCooldown = 250;
            ok = false;
        } else if (blockBreakCooldown > 0) {
            --blockBreakCooldown;
            if (blockBreakCooldown > 0) ok = false;
        }
    }

    if (ok && sv.limitToItems) {
        const std::string key = heldItemKey(env, player);
        if (key.find("sword") == std::string::npos) {
            debug::workerStage(key.empty() ? "weapon JNI returned no item key"
                                          : "held item rejected by sword filter");
            ok = false;
        }
    }

    env->DeleteLocalRef(player);
    env->DeleteLocalRef(mc);
    return ok;
}

}

namespace vanta::modules {

using aimassist::Runtime;

class AimAssist : public Module {
public:
    AimAssist() : Module("AimAssist",
                         "Pulls your aim onto the target",
                         Category::Combat) {
        registerSetting(&mode_);
        registerSetting(&requireClick_);
        registerSetting(&verticalAim_);
        registerSetting(&checkBlockBreak_);
        registerSetting(&horizontalSpeed_);
        registerSetting(&verticalSpeed_);
        registerSetting(&maxAngle_);
        registerSetting(&distance_);
        registerSetting(&targetArea_);
        registerSetting(&players_);
        registerSetting(&mobs_);
        registerSetting(&ignoreInvisible_);
        registerSetting(&onlyWeapons_);
        registerSetting(&focusMode_);
        registerSetting(&focusRange_);
        registerSetting(&debugFile_);
    }

    ~AimAssist() override {
        aimassist::g_rt.stop();
        JNIEnv* env = vanta::jvm::thisThreadEnv();
        if (env) aimassist::drop_cache(env);
    }

    void onInit() override {
        mirror();

        aimassist::g_rt.start();

        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){
            onFrame();
        });
    }

    void onEnable()  override {
        mirror();
        aimassist::g_rt.moduleEnabled.store(true,  std::memory_order_release);
        if (debugFile_.value()) {
            float sens = 0.5f;
            vanta::jvm::JniScope s;
            if (s) sens = aimassist::playerSensitivity(s.get());
            aimassist::debug::sessionOpen(mode_.index(), horizontalSpeed_.value(),
                                          verticalSpeed_.value(), verticalAim_.value(),
                                          requireClick_.value(), sens);
        }
    }
    void onDisable() override {
        auto* rt = &aimassist::g_rt;
        rt->moduleEnabled.store(false, std::memory_order_release);
        aimassist::debug::sessionClose();
        vanta::jvm::JniScope s;
        if (s) {
            aimassist::regular::onDisable(s.get(), rt->shared);
            aimassist::lockon::onDisable(s.get(), rt->shared);
        }
    }

    /* @no displayInfo, deliberately.

       This used to append the horizontal speed after the name, so the row read
       "AimAssist 15". A tuning number is not what the HUD is for: the row
       already carries the MODE beside it, which is the thing worth knowing at a
       glance, and the speed is something you set in the menu once. It was also
       the only override of displayInfo in the whole client, so the row was the
       odd one out for no reason a reader could see. */

private:

    EnumSetting  mode_            {this, "Mode", {"Regular", "Lock-on"}, 0};
    BoolSetting  requireClick_    {this, "Require click",   true };
    BoolSetting  verticalAim_     {this, "Vertical aim",    false};
    BoolSetting  checkBlockBreak_ {this, "Check block break", false};

    FloatSetting horizontalSpeed_ {this, "Horizontal speed", 15.0f, 1.0f, 30.0f, 1.0f};
    FloatSetting verticalSpeed_   {this, "Vertical speed",   10.0f, 1.0f, 30.0f, 0.1f};
    IntSetting   maxAngle_        {this, "Max angle",        180,  1,    360};

    FloatSetting distance_        {this, "Distance",         5.0f, 1.0f, 30.0f, 0.1f};

    EnumSetting  targetArea_      {this, "Target area", {"Head", "Chest", "Legs"}, 1};
    BoolSetting  players_         {this, "Players",          true};
    BoolSetting  mobs_            {this, "Mobs",             false};
    BoolSetting  ignoreInvisible_ {this, "Ignore invisible", false};
    BoolSetting  onlyWeapons_     {this, "Only weapons",     false};

    BoolSetting  focusMode_       {this, "Focus mode",       false};
    FloatSetting focusRange_      {this, "Focus range",      6.0f, 1.0f, 30.0f, 0.5f};
    /* @off, and in the list.

       It shipped on and unregistered: every enable truncated a file the user
       never asked for and had no switch to stop. A debug dump is something you
       turn on when you are debugging. */
    BoolSetting  debugFile_       {this, "Debug file",        false};
    FloatSetting emissionSmooth_  {this, "Emission smoothing", 5.0f, 0.0f, 10.0f, 0.5f};
    BoolSetting  jumpCompensate_  {this, "Jump compensate", false};

    aimassist::SettingsView mirror() {
        aimassist::SettingsView sv;
        sv.mode            = mode_.index();
        sv.requireMouseDown= requireClick_.value();
        sv.aimVertically   = verticalAim_.value();
        sv.strafeIncrease  = false;
        sv.checkBlockBreak = checkBlockBreak_.value();
        sv.horizontalSpeed = horizontalSpeed_.value();
        sv.verticalSpeed   = verticalSpeed_.value();
        sv.maxAngle        = maxAngle_.value();
        sv.distance        = distance_.value();

        sv.targetMode      = 0;
        sv.targetArea      = targetArea_.index();
        sv.players         = players_.value();
        sv.mobs            = mobs_.value();
        sv.ignoreInvisible = ignoreInvisible_.value();
        sv.limitToItems    = onlyWeapons_.value();
        sv.emissionSmooth  = emissionSmooth_.value();
        sv.jumpCompensate  = jumpCompensate_.value();
        sv.humanize        = false;
        sv.focusMode       = focusMode_.value();
        sv.focusRange      = focusRange_.value();

        aimassist::debug::opts(sv.mode, sv.horizontalSpeed,
                               sv.verticalSpeed, sv.aimVertically,
                               sv.requireMouseDown);

        {
            std::scoped_lock lk(aimassist::g_rt.settingsMx);
            aimassist::g_rt.settings = sv;
        }
        return sv;
    }

    void onFrame() {
        if (!isEnabled()) return;
        auto report = [](const char* stage) {
            auto& rt = aimassist::g_rt;
            aimassist::debug::frameStatus(stage, rt.running.load(std::memory_order_acquire),
                                         rt.moduleEnabled.load(std::memory_order_acquire));
        };

        vanta::jvm::JniScope s;
        if (!s) { report("no render JNIEnv"); return; }
        JNIEnv* env = s.get();
        if (!aimassist::ensure_cache(env)) { report("JNI cache unavailable"); return; }
        const aimassist::JniCache& c = aimassist::cache();
        if (!c.mc_instance_fid || !c.mc_player_fid || !c.ent_yaw_fid) {
            report("required render JNI fields missing"); return;
        }

        const aimassist::SettingsView sv = mirror();

        jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); report("Minecraft JNI exception"); return; }
        if (!mc) { report("no Minecraft instance"); return; }
        vanta::jvm::LocalRef<jobject> mcRef{env, mc};
        jobject player = env->GetObjectField(mc, c.mc_player_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); report("player JNI exception"); return; }
        if (!player) { report("no player"); return; }
        vanta::jvm::LocalRef<jobject> playerRef{env, player};

        auto& rt = aimassist::g_rt;

        /* Freecam freezes the player's rotation on purpose -- the view lives
           in the detached camera. An aim write here would turn the body and
           put that turn on the wire. */
        if (::vanta::vgui::bridge::isEnabled("Freecam")) { report("Freecam active"); return; }

        aimassist::debug::renderStage("applying aim");
        if (sv.mode == 1) aimassist::lockon::renderApply(env, player, rt.shared, sv);
        else              aimassist::regular::renderApply(env, player, rt.shared, sv);
        report(nullptr);
    }
};

}

VANTA_REGISTER_MODULE(vanta::modules::AimAssist)
