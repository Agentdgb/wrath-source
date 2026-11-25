#include "target_source.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dt::modules::targethud {

namespace {

struct jni_cache
{
    bool tried = false;
    bool ok = false;

    jclass mc_class = nullptr;
    jclass entity_class = nullptr;
    jclass living_class = nullptr;
    jclass player_class = nullptr;

    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_crosshair = nullptr;
    jfieldID  mc_player = nullptr;
    jfieldID  mc_world = nullptr;
    jmethodID mc_getRenderTickCounter = nullptr;
    jmethodID rtc_getTickProgress = nullptr;

    jclass    ehr_class = nullptr;
    jmethodID ehr_getEntity = nullptr;

    jmethodID ent_getX = nullptr;
    jmethodID ent_getY = nullptr;
    jmethodID ent_getZ = nullptr;
    jmethodID ent_isAlive = nullptr;
    jmethodID ent_isSpectator = nullptr;
    jmethodID ent_isRemoved = nullptr;
    jmethodID ent_getEyeHeight = nullptr;

    jfieldID ent_lastX = nullptr;
    jfieldID ent_lastY = nullptr;
    jfieldID ent_lastZ = nullptr;

    jmethodID liv_getHealth = nullptr;
    jmethodID liv_getMaxHealth = nullptr;
    jfieldID liv_swinging = nullptr;
    jfieldID liv_swingTicks = nullptr;

    jmethodID world_getPlayers = nullptr;
    jmethodID list_size = nullptr;
    jmethodID list_get = nullptr;

    jmethodID player_getProfile = nullptr;
    jmethodID profile_getName = nullptr;
};

jni_cache g_cache{};

target_snapshot g_snapshot{};

/* @who we are fighting, by name.

   A name and not a jobject: a jobject cannot be held across frames — it is a
   local reference that dies with the frame, and a global one would pin an entity
   the world may have unloaded. The name is what the panel shows anyway, and
   re-finding its owner in the player list once a frame is a handful of JNI calls
   for the one entity that matters. */
std::string g_locked{};

float g_hold = 0.f;      /* seconds of lock left before it expires */
float g_presence = 0.f;

/* @one swing, one decision.

   The latch is what stops an animation that stays in its opening state across
   several rendered frames from being read as several attacks. It clears when the
   swing advances or ends, so clicking fast still registers every click. */
bool g_swing_latched = false;

jclass global_class(JNIEnv* env, const char* sig)
{
    jclass local = dt::jvm::findClassAnyLoader(env, sig);

    if (local == nullptr)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }

    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

/* Frees the class refs a failed attempt may already have taken, so a retry
   starts from nothing instead of leaking one global ref per attempt. */
void drop_cache(JNIEnv* env)
{
    jclass* const classes[] = {
        &g_cache.mc_class, &g_cache.entity_class, &g_cache.living_class,
        &g_cache.player_class, &g_cache.ehr_class,
    };

    for (jclass* c : classes)
    {
        if (*c != nullptr)
        {
            env->DeleteGlobalRef(*c);
            *c = nullptr;
        }
    }

    g_cache = jni_cache{};
}

bool init_cache(JNIEnv* env)
{
    if (g_cache.tried && g_cache.ok)
        return true;

    /* @a failed resolve is retried, not remembered forever.

       The first poll can land in the window before the game class loader can
       serve these classes — hitfx logs exactly that ("capture install: no env")
       and retries once a second. This used to latch `tried` on the first attempt
       and never look again, so losing that race killed the HUD for the whole
       session with one warn line to show for it. Rate-limited so a genuinely
       unresolvable mapping costs one attempt a second, not one a frame. */
    if (g_cache.tried)
    {
        using clock = std::chrono::steady_clock;

        static clock::time_point next_attempt{};

        const clock::time_point now = clock::now();

        if (now < next_attempt)
            return false;

        next_attempt = now + std::chrono::seconds(1);

        drop_cache(env);
    }

    g_cache.tried = true;

    using namespace sdk::mappings;

    g_cache.mc_class = global_class(env, minecraftclass_sig);
    g_cache.entity_class = global_class(env, "net/minecraft/class_1297");
    g_cache.living_class = global_class(env, "net/minecraft/class_1309");
    g_cache.player_class = global_class(env, "net/minecraft/class_1657");
    g_cache.ehr_class = global_class(env, entity_hit_result_class_sig);

    /* @Entity/LivingEntity/PlayerEntity are looked up by their raw intermediary
       names rather than through the mapping table, because the table does not
       carry them — the ESP does exactly the same for exactly the same reason
       (esp.cpp:44-50). Everything the table DOES carry goes through it, so a
       Mojang-mapped launcher keeps working for those. */
    if (g_cache.mc_class == nullptr || g_cache.entity_class == nullptr
        || g_cache.living_class == nullptr || g_cache.player_class == nullptr)
    {
        VLOG_WARN("targethud", "core classes did not resolve");
        return false;
    }

    /* getInstance is not in the mapping table; the ESP resolves it by its raw
       intermediary name for the same reason (esp.cpp:179). */
    g_cache.mc_getInstance = env->GetStaticMethodID(g_cache.mc_class,
        "method_1551", "()Lnet/minecraft/class_310;");
    g_cache.mc_crosshair = env->GetFieldID(g_cache.mc_class,
        crosshair_target_name, crosshair_target_sig);
    g_cache.mc_player = env->GetFieldID(g_cache.mc_class, player_name, player_sig);
    g_cache.mc_world = env->GetFieldID(g_cache.mc_class, world_name, world_sig);

    g_cache.mc_getRenderTickCounter = env->GetMethodID(g_cache.mc_class,
        "method_61966", "()Lnet/minecraft/class_9779;");

    if (jclass rtc = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_9779"))
    {
        g_cache.rtc_getTickProgress = env->GetMethodID(rtc, "method_60637", "(Z)F");
        env->DeleteLocalRef(rtc);
    }

    if (g_cache.ehr_class != nullptr)
    {
        g_cache.ehr_getEntity = env->GetMethodID(g_cache.ehr_class,
            entity_hit_result_get_entity_name, entity_hit_result_get_entity_sig);
    }

    g_cache.ent_getX = env->GetMethodID(g_cache.entity_class, "method_23317", "()D");
    g_cache.ent_getY = env->GetMethodID(g_cache.entity_class, "method_23318", "()D");
    g_cache.ent_getZ = env->GetMethodID(g_cache.entity_class, "method_23321", "()D");
    g_cache.ent_isAlive = env->GetMethodID(g_cache.entity_class, "method_5805", "()Z");
    g_cache.ent_isSpectator = env->GetMethodID(g_cache.entity_class, "method_7325", "()Z");
    g_cache.ent_isRemoved = env->GetMethodID(g_cache.entity_class, "method_31481", "()Z");
    g_cache.ent_getEyeHeight = env->GetMethodID(g_cache.entity_class, "method_5751", "()F");

    /* lastRenderX/Y/Z — the tick-start position the game itself lerps from when
       it draws the model. Reading these and doing the same lerp is what puts the
       HUD exactly where the player is DRAWN rather than where the last packet
       said they were. */
    g_cache.ent_lastX = env->GetFieldID(g_cache.entity_class, "field_6038", "D");
    g_cache.ent_lastY = env->GetFieldID(g_cache.entity_class, "field_5971", "D");
    g_cache.ent_lastZ = env->GetFieldID(g_cache.entity_class, "field_5989", "D");

    g_cache.liv_getHealth = env->GetMethodID(g_cache.living_class,
        living_entity_get_health_name, living_entity_get_health_sig);
    g_cache.liv_getMaxHealth = env->GetMethodID(g_cache.living_class,
        living_entity_get_max_health_name, living_entity_get_max_health_sig);

    /* @the swing's own state, not its animation progress.

       handSwingProgress is a lerped 0..1 ramp that only moves on tick
       boundaries, so its first non-zero sample lands a full tick — up to 50ms —
       after the attack, and by then the crosshair has moved. These two fields
       are the raw state: swingHand() sets handSwinging and resets the tick
       counter, so `swinging && ticks <= 0` is true on the attack tick itself and
       is visible on the very next rendered frame. */
    g_cache.liv_swinging = env->GetFieldID(g_cache.living_class,
        hand_swinging_name, hand_swinging_sig);
    g_cache.liv_swingTicks = env->GetFieldID(g_cache.living_class,
        hand_swing_ticks_name, hand_swing_ticks_sig);

    if (jclass world = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_638"))
    {
        g_cache.world_getPlayers = env->GetMethodID(world, "method_18456", "()Ljava/util/List;");
        env->DeleteLocalRef(world);
    }

    if (jclass list = env->FindClass("java/util/List"))
    {
        g_cache.list_size = env->GetMethodID(list, "size", "()I");
        g_cache.list_get = env->GetMethodID(list, "get", "(I)Ljava/lang/Object;");
        env->DeleteLocalRef(list);
    }

    g_cache.player_getProfile = env->GetMethodID(g_cache.player_class,
        player_get_gameprofile_name, player_get_gameprofile_sig);

    if (jclass profile = dt::jvm::findClassAnyLoader(env, gameprofile_class_sig))
    {
        /* GameProfile is authlib, not Minecraft — its method name is the real
           one on every launcher, mapped or not. This is why the username comes
           from the profile instead of Entity.getName(): that returns a Text,
           and turning a Text into a string means another obfuscated hop for a
           value already sitting here as a plain java.lang.String. */
        g_cache.profile_getName = env->GetMethodID(profile,
            gameprofile_get_name_name, gameprofile_get_name_sig);
        env->DeleteLocalRef(profile);
    }

    if (env->ExceptionCheck()) env->ExceptionClear();

    /* Every id the sample path calls unguarded has to be in here. Y and Z were
       missing: a partial resolve — X found, Y not — left ok true and put a null
       jmethodID into CallDoubleMethod, which HotSpot dereferences directly. That
       is an access violation on the render thread, not a JNI error you can
       check for. */
    g_cache.ok = g_cache.mc_getInstance != nullptr
        && g_cache.mc_crosshair != nullptr
        && g_cache.ent_getX != nullptr
        && g_cache.ent_getY != nullptr
        && g_cache.ent_getZ != nullptr
        && g_cache.liv_getHealth != nullptr;

    /* @the swing pair gets its own line, because it is the whole feature.

       It is deliberately NOT in the `ok` conjunction above, and that is the
       right call: a null there should degrade the HUD, not kill it. The cost
       of the choice was three rounds of "it just does not appear" -- the
       handSwinging mapping named handSwingProgress, GetFieldID rejected it on
       the descriptor, `opening` was false on every frame that has ever run,
       and nothing anywhere said so. Every other module in this client prints
       its cache; now so does this one. */
    /* Once per outcome, not once per attempt: the resolve is retried every second
       now, and a line a second saying the same thing buries the log it is meant to
       make readable. */
    static int last_logged = -1;
    const int outcome = (g_cache.ok ? 1 : 0) | (g_cache.liv_swinging != nullptr ? 2 : 0);

    if (outcome == last_logged)
        return g_cache.ok;

    last_logged = outcome;

    VLOG_INFO("targethud", "cache: swing={} ticks={} players={} profile={} tick={} -> {}",
        g_cache.liv_swinging != nullptr ? 1 : 0,
        g_cache.liv_swingTicks != nullptr ? 1 : 0,
        g_cache.world_getPlayers != nullptr ? 1 : 0,
        (g_cache.player_getProfile != nullptr && g_cache.profile_getName != nullptr) ? 1 : 0,
        g_cache.rtc_getTickProgress != nullptr ? 1 : 0,
        g_cache.ok ? "ready" : "incomplete");

    if (!g_cache.ok)
        VLOG_WARN("targethud", "reflection incomplete, hud will stay empty");
    else if (g_cache.liv_swinging == nullptr || g_cache.liv_swingTicks == nullptr)
        VLOG_WARN("targethud", "swing fields unresolved - hits cannot be detected");


    return g_cache.ok;
}

/* @every local ref freed on every path.

   This is called once per player in the getPlayers walk, and PushLocalFrame's
   capacity is a guaranteed minimum rather than a cap — the JVM quietly chains
   more handle blocks instead of failing. So leaking here is not a crash, it is
   a growing handle chain rebuilt every frame, and a checked-JNI run that
   suddenly starts complaining. The ESP's read_jstr frees the same pair for the
   same reason. */
std::string read_name(JNIEnv* env, jobject entity)
{
    if (g_cache.player_getProfile == nullptr || g_cache.profile_getName == nullptr)
        return {};

    jobject profile = env->CallObjectMethod(entity, g_cache.player_getProfile);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (profile == nullptr) return {};

    jstring name = static_cast<jstring>(env->CallObjectMethod(profile, g_cache.profile_getName));

    env->DeleteLocalRef(profile);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    if (name == nullptr) return {};

    const char* chars = env->GetStringUTFChars(name, nullptr);

    if (chars == nullptr)
    {
        env->DeleteLocalRef(name);
        return {};
    }

    std::string out{ chars };

    env->ReleaseStringUTFChars(name, chars);
    env->DeleteLocalRef(name);

    return out;
}

/* Returns the entity the crosshair is on, or nullptr. Local ref — the caller is
   inside a local frame, so it does not have to be freed by hand. */
jobject crosshair_entity(JNIEnv* env, jobject mc)
{
    if (g_cache.mc_crosshair == nullptr || g_cache.ehr_getEntity == nullptr)
        return nullptr;

    jobject hit = env->GetObjectField(mc, g_cache.mc_crosshair);

    if (hit == nullptr)
        return nullptr;

    /* An instance check rather than reading the hit type: a BlockHitResult and a
       MISS both fail it, which is the same three-way answer for one JNI call
       instead of three. */
    if (g_cache.ehr_class == nullptr
        || env->IsInstanceOf(hit, g_cache.ehr_class) != JNI_TRUE)
        return nullptr;

    jobject entity = env->CallObjectMethod(hit, g_cache.ehr_getEntity);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    return entity;
}

/* Fills `out` from one already-identified player entity. Everything that decides
   WHICH entity that is happens in poll(). */
bool read_entity(JNIEnv* env, jobject entity, jobject self, float progress, target_snapshot& out)
{
    if (g_cache.ent_isAlive != nullptr
        && env->CallBooleanMethod(entity, g_cache.ent_isAlive) != JNI_TRUE)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    if (g_cache.ent_isSpectator != nullptr
        && env->CallBooleanMethod(entity, g_cache.ent_isSpectator) == JNI_TRUE)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    if (g_cache.ent_isRemoved != nullptr
        && env->CallBooleanMethod(entity, g_cache.ent_isRemoved) == JNI_TRUE)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    out.health = env->CallFloatMethod(entity, g_cache.liv_getHealth);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (out.health <= 0.0f) return false;

    out.max_health = g_cache.liv_getMaxHealth != nullptr
        ? env->CallFloatMethod(entity, g_cache.liv_getMaxHealth)
        : 20.f;

    if (env->ExceptionCheck()) { env->ExceptionClear(); out.max_health = 20.f; }

    if (!(out.max_health > 0.f))
        out.max_health = 20.f;

    out.health = std::clamp(out.health, 0.f, out.max_health);

    const double nx = env->CallDoubleMethod(entity, g_cache.ent_getX);
    const double ny = env->CallDoubleMethod(entity, g_cache.ent_getY);
    const double nz = env->CallDoubleMethod(entity, g_cache.ent_getZ);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    if (g_cache.ent_lastX != nullptr && g_cache.ent_lastY != nullptr && g_cache.ent_lastZ != nullptr)
    {
        const double px = env->GetDoubleField(entity, g_cache.ent_lastX);
        const double py = env->GetDoubleField(entity, g_cache.ent_lastY);
        const double pz = env->GetDoubleField(entity, g_cache.ent_lastZ);

        out.x = px + (nx - px) * progress;
        out.y = py + (ny - py) * progress;
        out.z = pz + (nz - pz) * progress;
    }
    else
    {
        out.x = nx;
        out.y = ny;
        out.z = nz;
    }

    out.height = 1.8f;

    if (g_cache.ent_getEyeHeight != nullptr)
    {
        const float eye = env->CallFloatMethod(entity, g_cache.ent_getEyeHeight);

        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (eye > 0.1f) out.height = eye;
    }

    out.distance = 0.f;

    {
        const double sx = env->CallDoubleMethod(self, g_cache.ent_getX);
        const double sy = env->CallDoubleMethod(self, g_cache.ent_getY);
        const double sz = env->CallDoubleMethod(self, g_cache.ent_getZ);

        if (env->ExceptionCheck()) env->ExceptionClear();
        else
        {
            const double dx = out.x - sx;
            const double dy = out.y - sy;
            const double dz = out.z - sz;

            out.distance = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }


    out.valid = true;
    return true;
}

/* Current frame's tick interpolation, or 1 when the counter is unavailable. */
float tick_progress(JNIEnv* env, jobject mc)
{
    if (g_cache.mc_getRenderTickCounter == nullptr || g_cache.rtc_getTickProgress == nullptr)
        return 1.f;

    jobject rtc = env->CallObjectMethod(mc, g_cache.mc_getRenderTickCounter);

    /* Checked BEFORE the null test: on a throw the returned jobject is undefined
       and in practice null, so the null branch is exactly the path that would
       swallow it and return with a throwable still pending — which makes the
       next JNI call illegal. */
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 1.f; }
    if (rtc == nullptr) return 1.f;

    const float progress = env->CallFloatMethod(rtc, g_cache.rtc_getTickProgress, JNI_FALSE);

    env->DeleteLocalRef(rtc);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return 1.f; }

    return std::clamp(progress, 0.f, 1.f);
}

/* Finds the locked player again in the world. Returns a local ref or nullptr. */
jobject find_locked(JNIEnv* env, jobject world)
{
    if (g_locked.empty() || g_cache.world_getPlayers == nullptr
        || g_cache.list_size == nullptr || g_cache.list_get == nullptr)
        return nullptr;

    jobject players = env->CallObjectMethod(world, g_cache.world_getPlayers);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (players == nullptr) return nullptr;

    /* getPlayers() rather than getEntities(): it is already filtered, and the
       full entity list costs a JNI round trip per dropped item and arrow in the
       world to find one player. */
    const jint count = env->CallIntMethod(players, g_cache.list_size);

    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(players); return nullptr; }

    jobject found = nullptr;

    for (jint index = 0; index < count && found == nullptr; ++index)
    {
        jobject entity = env->CallObjectMethod(players, g_cache.list_get, index);

        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (entity == nullptr) continue;

        if (read_name(env, entity) == g_locked)
            found = entity;
        else
            env->DeleteLocalRef(entity);
    }

    env->DeleteLocalRef(players);

    return found;
}

} // namespace

void poll(float delta, float linger, float range)
{
    bool fresh = false;
    bool struck = false;
    bool in_world = false;

    /* Set when the locked target was found this frame but is now further away
       than the range allows. Acted on below, next to the linger, because both
       answer the same question: is this still the fight you are in? */
    bool out_of_range = false;

    {
        dt::jvm::JniScope scope;

        if (scope && init_cache(scope.get()))
        {
            JNIEnv* env = scope.get();

            /* 32 is generous for one entity read plus a walk of the player list,
               and a frame that fails to open must NOT be popped — popping a frame
               that was never pushed corrupts the stack. */
            if (env->PushLocalFrame(32) == 0)
            {
                jobject mc = env->CallStaticObjectMethod(g_cache.mc_class, g_cache.mc_getInstance);

                if (env->ExceptionCheck()) env->ExceptionClear();

                jobject world = (mc != nullptr && g_cache.mc_world != nullptr)
                    ? env->GetObjectField(mc, g_cache.mc_world) : nullptr;

                jobject self = (mc != nullptr && g_cache.mc_player != nullptr)
                    ? env->GetObjectField(mc, g_cache.mc_player) : nullptr;

                if (world != nullptr && self != nullptr)
                {
                    in_world = true;

                    const float progress = tick_progress(env, mc);

                    /* @did we just swing at somebody?

                       The truthful signal would be MinecraftClient.doAttack, but
                       that hook belongs to the TriggerBot and only exists once
                       that module has run — a HUD that works depending on which
                       combat module happens to be on is not a HUD that works.

                       So: the local player's swing state, on its opening edge,
                       with the crosshair on a player. That is the same pair of
                       facts the game itself uses to decide an attack — doAttack
                       hits whatever crosshairTarget is holding — read rather than
                       intercepted, and read on the tick the swing STARTS so the
                       crosshair has not had time to move off. */
                    bool swinging = false;
                    jint swing_ticks = 1;

                    if (g_cache.liv_swinging != nullptr && g_cache.liv_swingTicks != nullptr)
                    {
                        swinging = env->GetBooleanField(self, g_cache.liv_swinging) == JNI_TRUE;
                        swing_ticks = env->GetIntField(self, g_cache.liv_swingTicks);
                    }

                    const bool opening = swinging && swing_ticks <= 0;

                    if (!opening)
                        g_swing_latched = false;

                    if (opening && !g_swing_latched)
                    {
                        /* Latched here and not on success, so one animation is one
                           decision either way. Latching only on a hit left a swing
                           at air free to lock onto whoever the crosshair reached
                           next, which is how you end up watching the health of
                           someone you never touched. */
                        g_swing_latched = true;

                        jobject aimed = crosshair_entity(env, mc);

                        if (aimed != nullptr
                            && env->IsInstanceOf(aimed, g_cache.player_class) == JNI_TRUE
                            && env->IsSameObject(self, aimed) != JNI_TRUE)
                        {
                            const bool spec = g_cache.ent_isSpectator && env->CallBooleanMethod(aimed, g_cache.ent_isSpectator) == JNI_TRUE;
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            const bool alive = !g_cache.ent_isAlive || env->CallBooleanMethod(aimed, g_cache.ent_isAlive) == JNI_TRUE;
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (!spec && alive)
                            {
                                std::string name = read_name(env, aimed);

                                if (!name.empty())
                                {
                                    g_locked = std::move(name);
                                    struck = true;
                                }
                            }
                        }
                    }

                    /* @and then follow them, whatever the crosshair is doing.

                       Re-found by name every frame rather than remembered as a
                       snapshot: in a melee the crosshair is off them most of the
                       time, and a panel frozen at the health they had when you
                       last looked is worse than no panel — it says something
                       false with confidence. */
                    if (!g_locked.empty())
                    {
                        jobject entity = find_locked(env, world);

                        if (entity != nullptr)
                        {
                            target_snapshot candidate{};
                            candidate.name = g_locked;

                            if (read_entity(env, entity, self, progress, candidate))
                            {
                                /* @too far is the same answer as gone.

                                   Checked here, on a freshly read distance,
                                   rather than against the stored snapshot: the
                                   snapshot is what survives when they cannot be
                                   found at all, and judging THAT by range would
                                   cut the lock on a frozen number. */
                                if (range > 0.f && candidate.distance > range)
                                    out_of_range = true;
                                else
                                {
                                    g_snapshot = std::move(candidate);
                                    fresh = true;
                                }
                            }
                            else
                            {
                                g_locked.clear();
                                g_hold = 0.f;
                            }
                        }
                    }
                }

                env->PopLocalFrame(nullptr);
            }
        }
    }

    /* @no world, nothing to remember.

       The lock and its snapshot are deliberately allowed to outlive the crosshair
       — that is the whole feature — but not to outlive the world they came from.
       Without this, disconnecting mid-fight left the last server's target on the
       multiplayer menu, at full opacity and forever in Always mode, because that
       mode is defined as holding the last target and nothing ever told it the
       target no longer exists. */
    if (!in_world)
    {
        g_snapshot = target_snapshot{};
        g_locked.clear();
        g_hold = 0.f;
        g_swing_latched = false;
    }

    /* The linger is re-armed by a HIT, not by seeing them. Tracking someone for
       four seconds is the point; tracking them forever because they stayed in
       front of you is not. */
    if (struck)
    {
        g_hold = std::max(linger, 0.f);
    }
    else if (g_hold > 0.f)
    {
        g_hold = std::max(0.f, g_hold - delta);
    }

    /* @and walking away ends it too, not just waiting.

       The lock was bounded only by time, so someone who ran kept the panel for
       the whole linger however far they got — a fixed-size card pinned beside a
       player who is now a handful of pixels tall, which is exactly what reads as
       the panel getting bigger. The reference client does not need a rule for
       this because its target provider drops anyone past ten blocks before the
       HUD ever sees them; this is that cut, made adjustable.

       A landed hit still wins. Reach is a few blocks and the range floor is
       four, so the two cannot really disagree — but if they ever do, the thing
       you just hit is the thing you are fighting. */
    if (out_of_range && !struck)
        g_hold = 0.f;

    if (g_hold <= 0.f)
    {
        g_locked.clear();
        g_snapshot.valid = false;
    }
    else if (!fresh)
    {
        /* Locked but not found this frame — they walked out of render distance,
           or into another dimension. The last known state stands for the rest of
           the linger rather than blinking out. */
    }

    const float target = g_hold > 0.f ? 1.f : 0.f;
    const float rate = target > g_presence ? 14.f : 9.f;

    g_presence += (target - g_presence) * std::clamp(rate * delta, 0.f, 1.f);

    if (g_presence < 0.002f)
        g_presence = 0.f;
}

const target_snapshot& latest()
{
    return g_snapshot;
}

float presence()
{
    return g_presence;
}

void reset()
{
    g_snapshot = target_snapshot{};
    g_locked.clear();
    g_hold = 0.f;
    g_presence = 0.f;
    g_swing_latched = false;
}

} // namespace dt::modules::targethud
