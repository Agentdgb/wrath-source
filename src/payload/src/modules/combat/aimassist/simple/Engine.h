#pragma once
#include "AimModel.h"
#include "MouseSensitivity.h"
#include "modules/shared/NativeAccess.h"
#include "core/social/EntityFriends.h"
#include "jvm/JvmContext.h"
#include <string>

namespace vanta::modules::aimassist::simple {
using readableaim::Settings;
class Engine {
    static constexpr const char *Client = "net/minecraft/class_310";
    static constexpr const char *Entity = "net/minecraft/class_1297";
    static constexpr const char *Living = "net/minecraft/class_1309";
    using Point = readableaim::Point;
    using Bounds = readableaim::Bounds;
    using Angles = readableaim::Angles;
    native::Access access;
    jobject worldRef = nullptr, targetRef = nullptr;
    readableaim::AimEngine engine;
    readableaim::MouseSensitivityOutput mouseOutput;
    Point randomPoint{0.5, 0.65, 0.5};
    Point nextRandomPoint{0.5, 0.65, 0.5};
    jint lastAttackTicks = -1;
    double lastFrame = 0;
    bool discardOnResume = false, modeInitialized = false;
    readableaim::AimMode lastMode = readableaim::AimMode::Simple;
    const char *state = "Disabled";
    std::string failure;
    double lastEatPauseTime = 0;
    float lastViewYaw = 0.0f;
    bool hasLastViewYaw = false;
    bool hitLocked = false;
    Point smoothedVelocity{};

    Point vector(jobject object) {
        if (!object)
            throw native::Failure("Missing vector");
        auto type = access.cls("net/minecraft/class_243");
        Point result{access.e->GetDoubleField(object, access.field(type, "field_1352", "D")),
                     access.e->GetDoubleField(object, access.field(type, "field_1351", "D")),
                     access.e->GetDoubleField(object, access.field(type, "field_1350", "D"))};
        access.check();
        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
            throw native::Failure("Invalid game vector");
        return result;
    }
    jobject vector(Point point) { return access.make("net/minecraft/class_243", "(DDD)V", point.x, point.y, point.z); }
    Point position(jobject entity) {
        const auto value = native::position(access, entity);
        return {value.x, value.y, value.z};
    }
    Angles camera(jobject player) {
        Angles result{access.call<jfloat>(player, Entity, "method_36454", "()F"),
                      access.call<jfloat>(player, Entity, "method_36455", "()F")};
        if (!std::isfinite(result.yaw) || !std::isfinite(result.pitch))
            throw native::Failure("Invalid camera angles");
        return result;
    }
    Bounds bounds(jobject entity) {
        auto object = access.call<>(entity, Entity, "method_5829", "()Lnet/minecraft/class_238;");
        auto type = access.cls("net/minecraft/class_238");
        const auto value = [&](const char *id) {
            const double number = access.e->GetDoubleField(object, access.field(type, id, "D"));
            access.check();
            return number;
        };
        Bounds box{{value("field_1323"), value("field_1322"), value("field_1321")},
                   {value("field_1320"), value("field_1325"), value("field_1324")}};
        if (!std::isfinite(box.minimum.x) || !std::isfinite(box.minimum.y) || !std::isfinite(box.minimum.z) ||
            !std::isfinite(box.maximum.x) || !std::isfinite(box.maximum.y) || !std::isfinite(box.maximum.z) ||
            box.maximum.x - box.minimum.x < 0.06 || box.maximum.y - box.minimum.y < 0.06 ||
            box.maximum.z - box.minimum.z < 0.06)
            throw native::Failure("Invalid entity bounding box");
        return box;
    }
    Point aimPoint(jobject entity, Point eye, Angles view, const Settings &settings, double elapsed = 0.05) {
        const Bounds box = bounds(entity);
        Point velocity{};
        if (settings.prediction > 0) {
            Point raw = vector(access.call<>(entity, Entity, "method_18798", "()Lnet/minecraft/class_243;"));
            if (targetRef && access.e->IsSameObject(entity, targetRef)) {
                const double vBlend = elapsed > 0 ? -std::expm1(-elapsed / 0.09) : 1.0;
                smoothedVelocity.x = std::lerp(smoothedVelocity.x, raw.x, vBlend);
                smoothedVelocity.y = std::lerp(smoothedVelocity.y, raw.y, vBlend);
                smoothedVelocity.z = std::lerp(smoothedVelocity.z, raw.z, vBlend);
                velocity = smoothedVelocity;
            } else {
                velocity = raw;
            }
        }
        return readableaim::constrainedAimPoint(box, velocity, eye, view, settings, randomPoint);
    }
    bool categoryAllowed(jobject entity, const Settings &settings) {
        if (access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1657")))
            return settings.players;
        if (access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1511")))
            return settings.crystals;
        if (access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1646")))
            return settings.villagers;
        if (access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1429")))
            return settings.animals;
        return access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1308")) && settings.mobs;
    }
    bool isRemovedSafe(jobject entity) {
        if (!entity) return false;
        try {
            auto cls = access.cls(Entity);
            auto mid = access.e->GetMethodID(cls, "method_31481", "()Z");
            if (access.e->ExceptionCheck()) {
                access.e->ExceptionClear();
                return false;
            }
            if (!mid) return false;
            return access.e->CallBooleanMethod(entity, mid) == JNI_TRUE;
        } catch (...) {
            return false;
        }
    }
    bool isAliveSafe(jobject entity) {
        if (!entity) return false;
        try {
            auto entityCls = access.cls(Entity);
            auto isAliveMid = access.e->GetMethodID(entityCls, "method_5805", "()Z");
            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
            if (isAliveMid && !access.e->CallBooleanMethod(entity, isAliveMid))
                return false;

            auto isRemovedMid = access.e->GetMethodID(entityCls, "method_31481", "()Z");
            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
            if (isRemovedMid && access.e->CallBooleanMethod(entity, isRemovedMid))
                return false;

            auto isSpectatorMid = access.e->GetMethodID(entityCls, "method_7325", "()Z");
            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
            if (isSpectatorMid && access.e->CallBooleanMethod(entity, isSpectatorMid))
                return false;

            auto isInvulnMid = access.e->GetMethodID(entityCls, "method_5682", "()Z");
            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
            if (isInvulnMid && access.e->CallBooleanMethod(entity, isInvulnMid))
                return false;

            auto livingCls = access.cls(Living);
            if (access.e->IsInstanceOf(entity, livingCls)) {
                auto hpMid = access.e->GetMethodID(livingCls, "method_6032", "()F");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (hpMid && access.e->CallFloatMethod(entity, hpMid) <= 0.0f)
                    return false;

                auto dtFid = access.e->GetFieldID(livingCls, "field_6213", "I");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (!dtFid) {
                    dtFid = access.e->GetFieldID(livingCls, "deathTime", "I");
                    if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                }
                if (dtFid && access.e->GetIntField(entity, dtFid) > 0)
                    return false;

                auto isDeadMid = access.e->GetMethodID(livingCls, "method_29504", "()Z");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (isDeadMid && access.e->CallBooleanMethod(entity, isDeadMid))
                    return false;

                auto isSleepingMid = access.e->GetMethodID(livingCls, "method_6109", "()Z");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (isSleepingMid && access.e->CallBooleanMethod(entity, isSleepingMid))
                    return false;
            }

            auto playerCls = access.cls("net/minecraft/class_1657");
            if (access.e->IsInstanceOf(entity, playerCls)) {
                auto isCreativeMid = access.e->GetMethodID(playerCls, "method_7337", "()Z");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (!isCreativeMid) {
                    isCreativeMid = access.e->GetMethodID(playerCls, "isCreative", "()Z");
                    if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                }
                if (isCreativeMid && access.e->CallBooleanMethod(entity, isCreativeMid))
                    return false;

                auto abilitiesFid = access.e->GetFieldID(playerCls, "field_7503", "Lnet/minecraft/class_1656;");
                if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                if (!abilitiesFid) {
                    abilitiesFid = access.e->GetFieldID(playerCls, "abilities", "Lnet/minecraft/class_1656;");
                    if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                }
                if (abilitiesFid) {
                    jobject abilities = access.e->GetObjectField(entity, abilitiesFid);
                    if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                    if (abilities) {
                        auto abCls = access.cls("net/minecraft/class_1656");
                        auto flyFid = access.e->GetFieldID(abCls, "field_7479", "Z");
                        if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                        if (!flyFid) {
                            flyFid = access.e->GetFieldID(abCls, "flying", "Z");
                            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                        }
                        const bool flying = flyFid && access.e->GetBooleanField(abilities, flyFid);

                        auto allowFlyFid = access.e->GetFieldID(abCls, "field_7478", "Z");
                        if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                        if (!allowFlyFid) {
                            allowFlyFid = access.e->GetFieldID(abCls, "allowFlying", "Z");
                            if (access.e->ExceptionCheck()) access.e->ExceptionClear();
                        }
                        const bool allowFlying = allowFlyFid && access.e->GetBooleanField(abilities, allowFlyFid);

                        access.e->DeleteLocalRef(abilities);
                        if (flying || allowFlying)
                            return false;
                    }
                }
            }
        } catch (...) {
            if (access.e && access.e->ExceptionCheck()) access.e->ExceptionClear();
        }
        return true;
    }
    bool trackable(jobject player, jobject entity, const Settings &settings, bool allowInvisible = false) {
        if (!entity || access.e->IsSameObject(entity, player) ||
            !isAliveSafe(entity) ||
            access.call<jboolean>(entity, Entity, "method_7325", "()Z") ||
            isRemovedSafe(entity) ||
            (!allowInvisible && access.call<jboolean>(entity, Entity, "method_5767", "()Z")) ||
            !categoryAllowed(entity, settings) ||
            (access.e->IsInstanceOf(entity, access.cls("net/minecraft/class_1657")) &&
             (social::isFriend(access.e, entity) || (settings.teams && social::isTeammate(access.e, player, entity)))))
            return false;
        return true;
    }
    bool valid(jobject player, jobject entity, Point playerPosition, const Settings &settings, double range) {
        return trackable(player, entity, settings, false) &&
               readableaim::distanceSquared(playerPosition, position(entity)) <= range * range &&
               access.call<jboolean>(player, Living, "method_6057", "(Lnet/minecraft/class_1297;)Z", entity);
    }
    void dropTarget(JNIEnv *env) {
        if (targetRef)
            env->DeleteGlobalRef(targetRef);
        targetRef = nullptr;
        hitLocked = false;
        lastAttackTicks = -1;
        engine.reset();
        mouseOutput.reset();
        hasLastViewYaw = false;
        smoothedVelocity = {};
    }
    void adoptTarget(JNIEnv *env, jobject reference) {
        dropTarget(env);
        targetRef = reference;
        if (targetRef) {
            pickRandomPoint();
            randomPoint = nextRandomPoint;
        }
    }
    void pickRandomPoint() {
        nextRandomPoint = {readableaim::randomRange(0.46, 0.54),
                           readableaim::randomRange(0.58, 0.70),
                           readableaim::randomRange(0.46, 0.54)};
    }
    void choose(jobject world, jobject player, Point playerPosition, Point eye, Angles view,
                const Settings &settings) {
        jobject best = nullptr;
        double bestScore = std::numeric_limits<double>::infinity();
        try {
            auto list = access.call<>(world, "net/minecraft/class_638", "method_18112", "()Ljava/lang/Iterable;");
            auto iterator = access.call<>(list, "java/lang/Iterable", "iterator", "()Ljava/util/Iterator;");
            while (access.call<jboolean>(iterator, "java/util/Iterator", "hasNext", "()Z")) {
                native::Frame local(access.e);
                auto candidate = access.call<>(iterator, "java/util/Iterator", "next", "()Ljava/lang/Object;");
                if (!valid(player, candidate, playerPosition, settings, settings.distance))
                    continue;
                const double error = readableaim::angleError(
                    view, readableaim::anglesTo(eye, aimPoint(candidate, eye, view, settings)),
                    settings.vertical);
                if (error > settings.fov * 0.5)
                    continue;
                const double score = settings.priority == readableaim::Priority::Distance
                                         ? readableaim::distanceSquared(playerPosition, position(candidate))
                                         : error;
                if (score >= bestScore)
                    continue;
                auto replacement = access.e->NewGlobalRef(candidate);
                access.check();
                if (!replacement)
                    throw native::Failure("Cannot retain target");
                if (best)
                    access.e->DeleteGlobalRef(best);
                best = replacement;
                bestScore = score;
            }
        } catch (...) {
            if (best)
                access.e->DeleteGlobalRef(best);
            throw;
        }
        if (access.e->IsSameObject(best, targetRef)) {
            if (best)
                access.e->DeleteGlobalRef(best);
            return;
        }
        adoptTarget(access.e, best);
    }
    bool holdingWeapon(jobject player) {
        auto stack = access.call<>(player, Living, "method_6047", "()Lnet/minecraft/class_1799;");
        for (const char *id : {"field_42611", "field_42612"}) {
            auto tag = access.constant("net/minecraft/class_3489", id, "Lnet/minecraft/class_6862;");
            if (access.call<jboolean>(stack, "net/minecraft/class_1799", "method_31573",
                                      "(Lnet/minecraft/class_6862;)Z", tag))
                return true;
        }
        for (const char *id : {"field_49814", "field_8547"}) {
            auto item = access.constant("net/minecraft/class_1802", id, "Lnet/minecraft/class_1792;");
            if (access.call<jboolean>(stack, "net/minecraft/class_1799", "method_31574",
                                      "(Lnet/minecraft/class_1792;)Z", item))
                return true;
        }
        return false;
    }
    bool isEating(jobject player) {
        try {
            if (access.call<jboolean>(player, Living, "method_6115", "()Z"))
                return true;
        } catch (...) {
            if (access.e->ExceptionCheck())
                access.e->ExceptionClear();
        }
        const bool rightClick = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool leftClick = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (rightClick && !leftClick)
            return true;
        return false;
    }
    double mouseSensitivity(jobject client) {
        auto options = access.get(client, Client, "field_1690", "Lnet/minecraft/class_315;");
        auto option = access.call<>(options, "net/minecraft/class_315", "method_42495", "()Lnet/minecraft/class_7172;");
        auto value = access.call<>(option, "net/minecraft/class_7172", "method_41753", "()Ljava/lang/Object;");
        const double sensitivity = access.call<jdouble>(value, "java/lang/Double", "doubleValue", "()D");
        if (!std::isfinite(sensitivity) || sensitivity < 0 || sensitivity > 1)
            throw native::Failure("Invalid Minecraft mouse sensitivity");
        return sensitivity;
    }

  public:
    ~Engine() {
        jvm::JniScope scope;
        if (scope)
            clear(scope.get());
    }
    void clear(JNIEnv *env) {
        dropTarget(env);
        if (worldRef)
            env->DeleteGlobalRef(worldRef);
        worldRef = nullptr;
        lastFrame = 0;
        lastEatPauseTime = 0;
        discardOnResume = false;
        modeInitialized = false;
        failure.clear();
    }
    void releaseTarget(JNIEnv *env) {
        dropTarget(env);
    }
    void pause(const char *reason) {
        engine.reset();
        mouseOutput.reset();
        lastFrame = 0;
        discardOnResume = true;
        hasLastViewYaw = false;
        state = reason;
    }
    const char *status() const { return state; }
    const std::string &error() const { return failure; }
    jobject target() const { return targetRef; }
    void focusAttack(JNIEnv *env, jobject target, const Settings &settings) {
        if (!target) return;
        access.e = env;
        if (targetRef && env->IsSameObject(target, targetRef)) {
            hitLocked = true;
            return;
        }
        try {
            auto client = access.minecraft();
            auto player = access.get(client, Client, "field_1724", "Lnet/minecraft/class_746;");
            if (!player || !trackable(player, target, settings)) return;
            if (readableaim::distanceSquared(position(player), position(target)) > 8.0 * 8.0) return;
        } catch (...) {
            return;
        }
        auto reference = env->NewGlobalRef(target);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (reference) {
            adoptTarget(env, reference);
            hitLocked = true;
        }
    }
    void step(JNIEnv *env, Settings settings, double now, bool focusHeld = false) {
        access.e = env;
        settings.sanitize();
        const bool focusing = (settings.focusMode == readableaim::FocusMode::Auto ||
                              (settings.focusMode == readableaim::FocusMode::Keybind && focusHeld)) ||
                              settings.onlyOnHit;
        if (!modeInitialized || lastMode != settings.mode) {
            dropTarget(env);
            lastMode = settings.mode;
            modeInitialized = true;
            lastFrame = 0;
        }
        const double elapsed = lastFrame == 0 ? 0 : std::min(now - lastFrame, 0.05);
        lastFrame = now;
        try {
            native::Frame frame(env);
            if (discardOnResume) {
                if (!focusing) dropTarget(env);
                discardOnResume = false;
            }
            auto client = access.minecraft();
            if (!access.call<jboolean>(client, Client, "method_18854", "()Z")) {
                pause("Waiting for game thread");
                return;
            }
            auto world = access.get(client, Client, "field_1687", "Lnet/minecraft/class_638;");
            auto player = access.get(client, Client, "field_1724", "Lnet/minecraft/class_746;");
            if (!world || !player) {
                clear(env);
                pause("No world");
                return;
            }
            if (!env->IsSameObject(worldRef, world)) {
                dropTarget(env);
                if (worldRef)
                    env->DeleteGlobalRef(worldRef);
                worldRef = env->NewGlobalRef(world);
                access.check();
                if (!worldRef)
                    throw native::Failure("Cannot retain world");
            }
            if (!access.call<jboolean>(player, Entity, "method_5805", "()Z") ||
                access.call<jboolean>(player, Entity, "method_7325", "()Z")) {
                dropTarget(env);
                pause("Player unavailable");
                return;
            }
            if (access.call<jboolean>(client, Client, "method_1493", "()Z")) {
                pause("Game paused");
                return;
            }
            if (access.get(client, Client, "field_1755", "Lnet/minecraft/class_437;")) {
                pause("Menu open");
                return;
            }
            if (settings.onlyWeapon && !holdingWeapon(player)) {
                if (!focusing) dropTarget(env);
                pause("Weapon required");
                return;
            }
            if (settings.pauseOnEat && isEating(player)) {
                engine.reset();
                mouseOutput.reset();
                lastFrame = 0;
                lastEatPauseTime = now;
                state = "Eating / using item";
                return;
            }
            if (elapsed <= 0) {
                state = failure.empty() ? "Searching" : "Compatibility error";
                return;
            }
            const Point playerPosition = position(player),
                        eye = vector(access.call<>(player, Entity, "method_33571", "()Lnet/minecraft/class_243;"));
            const Angles view = camera(player);
            const bool isAlive = targetRef && trackable(player, targetRef, settings, true);
            const bool retained = focusing && targetRef && isAlive;
            if (!retained) {
                if (targetRef) dropTarget(env);
                if (!settings.onlyOnHit) {
                    choose(world, player, playerPosition, eye, view, settings);
                }
            }
            if (!targetRef) {
                state = settings.onlyOnHit ? "Waiting for hit" : "No target in range";
                return;
            }
            if (settings.position == readableaim::AimPosition::Random) {
                const auto playerClass = access.cls("net/minecraft/class_1657");
                const jint attackTicks = env->GetIntField(player, access.field(playerClass, "field_6273", "I"));
                access.check();
                if (lastAttackTicks >= 0 && attackTicks < lastAttackTicks)
                    pickRandomPoint();
                lastAttackTicks = attackTicks;
                const double blend = -std::expm1(-elapsed / 0.35);
                randomPoint = {std::lerp(randomPoint.x, nextRandomPoint.x, blend),
                               std::lerp(randomPoint.y, nextRandomPoint.y, blend),
                               std::lerp(randomPoint.z, nextRandomPoint.z, blend)};
            } else {
                lastAttackTicks = -1;
            }
            Angles desired = readableaim::anglesTo(eye, aimPoint(targetRef, eye, view, settings, elapsed));
            if (!settings.vertical)
                desired.pitch = view.pitch;

            const double currentError = readableaim::angleError(view, desired, settings.vertical);
            Settings activeSettings = settings;
            const double sinceEat = lastEatPauseTime > 0 ? (now - lastEatPauseTime) : 999.0;
            if (sinceEat < 0.60) {
                if (currentError > 30.0) {
                    engine.reset();
                    mouseOutput.reset();
                    lastFrame = now;
                    state = "Target retained outside assistance";
                    return;
                }
                const double ramp = std::clamp(sinceEat / 0.60, 0.20, 1.0);
                activeSettings.speed1 *= ramp;
            }
            // If hitLocked and beyond knockback range (16 blocks), pause assist until closing distance
            const double distSq = readableaim::distanceSquared(playerPosition, position(targetRef));
            constexpr double HITLOCK_MAX_ASSIST_DIST_SQ = 16.0 * 16.0;
            if (hitLocked && distSq > HITLOCK_MAX_ASSIST_DIST_SQ) {
                state = "Focus locked (closing distance)";
                return;
            }
            const bool outside = !trackable(player, targetRef, activeSettings) ||
                                 currentError > activeSettings.fov * 0.5;
            const auto result = engine.update(view, desired, activeSettings, elapsed, outside);
            Angles applied = view, actual = view;
            if (result.suspended) mouseOutput.reset();
            else applied = mouseOutput.apply(view, result.camera, desired, settings.vertical, mouseSensitivity(client));
            if (applied.yaw != view.yaw || (settings.vertical && applied.pitch != view.pitch)) {
                access.call<void>(player, Entity, "method_36456", "(F)V", applied.yaw);
                if (settings.vertical)
                    access.call<void>(player, Entity, "method_36457", "(F)V", applied.pitch);
                actual = camera(player);
                failure.clear();
            }
            lastViewYaw = view.yaw;
            hasLastViewYaw = true;
            engine.recordApplied(actual);
            state = !failure.empty()          ? "Compatibility error"
                    : result.manuallyReleased ? "Manual aim"
                    : outside                 ? "Target retained outside assistance"
                    : result.suspended        ? "Mouse input"
                                              : "Tracking";
        } catch (const native::Failure &error) {
            failure = error.what();
            pause("Compatibility error");
        }
    }
};
} // namespace vanta::modules::aimassist::simple::simple
