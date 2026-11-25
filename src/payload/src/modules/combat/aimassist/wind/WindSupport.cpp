

#include "aimassist.h"
#include "aimassist_debug.h"

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

namespace dt::modules::aimassist {

namespace {

void workerLoop(Runtime* rt)
{

#pragma comment(lib, "winmm.lib")
    const UINT timerPeriod = 1;
    const MMRESULT timerOk = timeBeginPeriod(timerPeriod);

    JavaVM* vm = nullptr;
    while (rt->running.load(std::memory_order_acquire) && !vm) {
        vm = dt::jvm::ctx().vm;
        if (!vm) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    if (!vm) { if (timerOk == TIMERR_NOERROR) timeEndPeriod(timerPeriod); return; }
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK
        || !env) {
        if (timerOk == TIMERR_NOERROR) timeEndPeriod(timerPeriod);
        return;
    }

    int blockBreakCooldown = 0;

    long long cadenceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int cadenceTicks = 0;
    bool cadenceLogged = false;

    while (rt->running.load(std::memory_order_acquire)) {
        {
        std::lock_guard<std::mutex> control(rt->control);
        if (rt->moduleEnabled.load(std::memory_order_acquire)) {
            debug::workerStage("entering worker tick");
            const SettingsView sv = rt->settings;

            regular::workerTick(env, rt->shared, sv, blockBreakCooldown);
        } else debug::workerStage("module disabled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        {
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
                          timerOk == TIMERR_NOERROR ? "1ms OK" : "FAILED");
                cadenceLogged = true;
            }
        }
    }
    vm->DetachCurrentThread();
    if (timerOk == TIMERR_NOERROR) timeEndPeriod(timerPeriod);
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
    if (c.ent_is_spectator_mid
        && env->CallBooleanMethod(target, c.ent_is_spectator_mid) == JNI_TRUE)
        return true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (c.ent_is_removed_mid
        && env->CallBooleanMethod(target, c.ent_is_removed_mid) == JNI_TRUE)
        return true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (c.living_health_mid) {
        const float hp = env->CallFloatMethod(target, c.living_health_mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (hp <= 0.f) return true;
    }
    if (c.living_class && env->IsInstanceOf(target, c.living_class) == JNI_TRUE) {
        jfieldID dtFid = env->GetFieldID(c.living_class, "field_6213", "I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!dtFid) {
            dtFid = env->GetFieldID(c.living_class, "deathTime", "I");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (dtFid && env->GetIntField(target, dtFid) > 0) return true;
    }
    if (c.player_class && env->IsInstanceOf(target, c.player_class) == JNI_TRUE) {
        jfieldID abFid = env->GetFieldID(c.player_class, "field_7503", "Lnet/minecraft/class_1656;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!abFid) {
            abFid = env->GetFieldID(c.player_class, "abilities", "Lnet/minecraft/class_1656;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (abFid) {
            jobject ab = env->GetObjectField(target, abFid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (ab) {
                jclass abCls = env->GetObjectClass(ab);
                jfieldID flyFid = env->GetFieldID(abCls, "field_7479", "Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                jfieldID allowFlyFid = env->GetFieldID(abCls, "field_7478", "Z");
                if (env->ExceptionCheck()) env->ExceptionClear();
                const bool flying = (flyFid && env->GetBooleanField(ab, flyFid) == JNI_TRUE) ||
                                    (allowFlyFid && env->GetBooleanField(ab, allowFlyFid) == JNI_TRUE);
                env->DeleteLocalRef(abCls);
                env->DeleteLocalRef(ab);
                if (flying) return true;
            }
        }
    }
    return false;
}

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
    if (env->IsSameObject(target, player) == JNI_TRUE) ok = false;

    if (ok) {

        if (c.living_class && env->IsInstanceOf(target, c.living_class) != JNI_TRUE) ok = false;
        else if (c.armor_stand_class
                 && env->IsInstanceOf(target, c.armor_stand_class) == JNI_TRUE) ok = false;
    }
    if (ok && isDeadOrDying(env, target)) ok = false;

    if (ok) {
        const double dist = distanceToEntity(env, player, target);
        if (dist > static_cast<double>(sv.distance)) ok = false;
        else {
            const EntPos pp = entityPos(env, player);
            const EntPos tp = entityPos(env, target);
            const float yaw = env->GetFloatField(player, c.ent_yaw_fid);
            if (angleToTarget(yaw, pp.x, pp.z, tp.x, tp.z)
                    > static_cast<int>(sv.maxAngle) / 2)
                ok = false;
        }
    }

    if (ok) {

        const bool isPlayer = isPlayerEntity(env, target);
        if (isPlayer && !sv.players) ok = false;
        if (!isPlayer && !sv.mobs) ok = false;
        // A friend is not a target. Cached per entity id — this runs on the
        // worker thread at ~1 kHz over every loaded entity.
        if (ok && isPlayer && ::dt::social::isFriend(env, target)) ok = false;
        if (ok && isPlayer && sv.teams && ::dt::social::isTeammate(env, player, target)) ok = false;
        if (ok && sv.ignoreInvisible && c.ent_invisible_mid
            && env->CallBooleanMethod(target, c.ent_invisible_mid) == JNI_TRUE)
            ok = false;
    }

    if (ok && sv.limitToItems) {
        const std::string key = heldItemKey(env, player);
        if (key.find("sword") == std::string::npos) ok = false;
    }

    if (player) env->DeleteLocalRef(player);
    if (mc) env->DeleteLocalRef(mc);
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
        if (!isValidTarget(env, ent, sv)) { env->DeleteLocalRef(ent); continue; }

        double key = 0.0;
        switch (sv.targetMode) {
            case 1:   key = distanceToEntity(env, player, ent); break;
            case 2:   {
                if (isPlayerEntity(env, ent))
                    key = equipmentScore(env, ent);
                else
                    key = distanceToEntity(env, player, ent);
                break;
            }
            case 3:   {
                if (isPlayerEntity(env, ent))
                    key = threatScore(env, ent);
                else
                    key = distanceToEntity(env, player, ent);
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
                const EntPos tp = entityPos(env, ent);
                key = angleToTarget(playerYaw, pp.x, pp.z, tp.x, tp.z);
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
