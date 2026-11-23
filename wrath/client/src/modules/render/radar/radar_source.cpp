#include "radar_source.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace vanta::modules::radar {

namespace {

struct jni_cache
{
    bool tried = false;
    bool ok = false;

    jclass mc_class = nullptr;
    jclass entity_class = nullptr;

    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_player = nullptr;
    jfieldID  mc_world = nullptr;
    jmethodID mc_getRenderTickCounter = nullptr;
    jmethodID rtc_getTickProgress = nullptr;

    jmethodID ent_getX = nullptr;
    jmethodID ent_getY = nullptr;
    jmethodID ent_getZ = nullptr;
    jmethodID ent_isAlive = nullptr;
    jmethodID ent_getYaw = nullptr;
    jmethodID ent_getYawLerped = nullptr;

    jfieldID ent_lastX = nullptr;
    jfieldID ent_lastY = nullptr;
    jfieldID ent_lastZ = nullptr;

    jmethodID world_getPlayers = nullptr;
    jmethodID list_size = nullptr;
    jmethodID list_get = nullptr;
};

jni_cache g_cache{};

std::vector<blip> g_blips{};

/* The viewer's own yaw from the last poll, kept so the dial can put north on the
   rim. Interpolated exactly like the bearings that are measured against it. */
float g_yaw = 0.f;

/* A crowded lobby can hold hundreds of players and every one of them costs a
   handful of JNI calls. The panel cannot show more than a few dozen blips
   legibly anyway, and the list is sorted nearest-first, so the cap costs
   nothing anyone can see. */
constexpr std::size_t k_max_blips = 64;

constexpr double k_pi = 3.14159265358979323846;

jclass global_class(JNIEnv* env, const char* sig)
{
    jclass local = vanta::jvm::findClassAnyLoader(env, sig);

    if (local == nullptr)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }

    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

void drop_cache(JNIEnv* env)
{
    jclass* const classes[] = { &g_cache.mc_class, &g_cache.entity_class };

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

    /* Retried once a second rather than latched on the first attempt. The first
       poll can land before the game's class loader will serve these classes, and
       a module that gives up there is dead for the whole session with one warn
       line to show for it. */
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

    if (g_cache.mc_class == nullptr || g_cache.entity_class == nullptr)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        VLOG_WARN("radar", "core classes did not resolve");
        return false;
    }

    g_cache.mc_getInstance = env->GetStaticMethodID(g_cache.mc_class,
        "method_1551", "()Lnet/minecraft/class_310;");
    g_cache.mc_player = env->GetFieldID(g_cache.mc_class, player_name, player_sig);
    g_cache.mc_world = env->GetFieldID(g_cache.mc_class, world_name, world_sig);

    g_cache.mc_getRenderTickCounter = env->GetMethodID(g_cache.mc_class,
        "method_61966", "()Lnet/minecraft/class_9779;");

    if (jclass rtc = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_9779"))
    {
        g_cache.rtc_getTickProgress = env->GetMethodID(rtc, "method_60637", "(Z)F");
        env->DeleteLocalRef(rtc);
    }

    g_cache.ent_getX = env->GetMethodID(g_cache.entity_class, "method_23317", "()D");
    g_cache.ent_getY = env->GetMethodID(g_cache.entity_class, "method_23318", "()D");
    g_cache.ent_getZ = env->GetMethodID(g_cache.entity_class, "method_23321", "()D");
    g_cache.ent_isAlive = env->GetMethodID(g_cache.entity_class, "method_5805", "()Z");

    g_cache.ent_getYaw = env->GetMethodID(g_cache.entity_class,
        entity_get_yaw_name, entity_get_yaw_sig);

    /* @the tick-delta overload, because every other term here is interpolated.
       Entity.getYaw() is the tick-END yaw, while the camera you are actually
       looking through is drawn at lerp(progress, lastYaw, yaw). Subtracting the
       un-lerped one from lerped positions shears the whole blip field by a
       frame's worth of turn and snaps it back each tick -- blips swimming around
       the dial exactly while you are turning, which is when a radar is read.
       Optional: left out of the `ok` conjunction below and null-tested at the
       call, so a launcher that cannot serve it degrades to the raw yaw rather
       than losing the module. */
    g_cache.ent_getYawLerped = env->GetMethodID(g_cache.entity_class,
        "method_5705", "(F)F");

    /* lastRenderX/Y/Z — the tick-start position the game itself lerps from when
       it draws the model. Doing the same lerp is what makes a blip glide instead
       of stepping twenty times a second. */
    g_cache.ent_lastX = env->GetFieldID(g_cache.entity_class, "field_6038", "D");
    g_cache.ent_lastY = env->GetFieldID(g_cache.entity_class, "field_5971", "D");
    g_cache.ent_lastZ = env->GetFieldID(g_cache.entity_class, "field_5989", "D");

    if (jclass world = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_638"))
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

    if (env->ExceptionCheck()) env->ExceptionClear();

    /* Everything the sample path calls unguarded. A partial resolve reported as
       success puts a null jmethodID into CallDoubleMethod, which HotSpot
       dereferences directly — an access violation on the render thread, not a
       JNI error anything can check for. */
    g_cache.ok = g_cache.mc_getInstance != nullptr
        && g_cache.mc_player != nullptr
        && g_cache.mc_world != nullptr
        && g_cache.ent_getX != nullptr
        && g_cache.ent_getY != nullptr
        && g_cache.ent_getZ != nullptr
        && g_cache.ent_getYaw != nullptr
        && g_cache.world_getPlayers != nullptr
        && g_cache.list_size != nullptr
        && g_cache.list_get != nullptr;

    /* Once per outcome, the way every other module in this client reports its
       cache — so a null id is one grep away instead of a silent empty panel. */
    static int last_logged = -1;
    const int outcome = g_cache.ok ? 1 : 0;

    if (outcome != last_logged)
    {
        last_logged = outcome;

        VLOG_INFO("radar", "cache: yaw={} lerp={} players={} tick={} -> {}",
            g_cache.ent_getYaw != nullptr ? 1 : 0,
            (g_cache.ent_lastX != nullptr && g_cache.ent_lastZ != nullptr) ? 1 : 0,
            g_cache.world_getPlayers != nullptr ? 1 : 0,
            g_cache.rtc_getTickProgress != nullptr ? 1 : 0,
            g_cache.ok ? "ready" : "incomplete");

        if (g_cache.ent_getYawLerped == nullptr)
            VLOG_WARN("radar", "interpolated yaw unavailable - blips will jitter while turning");
    }

    return g_cache.ok;
}

/* This frame's tick interpolation, or 1 when the counter is unavailable. */
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

/* Interpolated render position. Returns false on any JNI trouble. */
bool render_pos(JNIEnv* env, jobject entity, float progress, double& x, double& y, double& z)
{
    const double nx = env->CallDoubleMethod(entity, g_cache.ent_getX);
    const double ny = env->CallDoubleMethod(entity, g_cache.ent_getY);
    const double nz = env->CallDoubleMethod(entity, g_cache.ent_getZ);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    if (g_cache.ent_lastX != nullptr && g_cache.ent_lastY != nullptr && g_cache.ent_lastZ != nullptr)
    {
        const double px = env->GetDoubleField(entity, g_cache.ent_lastX);
        const double py = env->GetDoubleField(entity, g_cache.ent_lastY);
        const double pz = env->GetDoubleField(entity, g_cache.ent_lastZ);

        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

        x = px + (nx - px) * progress;
        y = py + (ny - py) * progress;
        z = pz + (nz - pz) * progress;
        return true;
    }

    x = nx; y = ny; z = nz;
    return true;
}

/* [-180, 180), so a blip never takes the long way round the dial. */
float wrap_degrees(double d)
{
    d = std::fmod(d + 180.0, 360.0);

    if (d < 0.0)
        d += 360.0;

    return static_cast<float>(d - 180.0);
}

} // namespace

void poll(float range)
{
    g_blips.clear();

    if (range <= 0.f)
        return;

    vanta::jvm::JniScope scope;

    if (!scope)
        return;

    JNIEnv* env = scope.get();

    if (!init_cache(env))
        return;

    /* Capacity is a guaranteed minimum rather than a cap — the JVM chains more
       handle blocks instead of failing — but the walk below takes a few refs per
       player, so asking for room up front keeps a crowded lobby from rebuilding
       that chain every frame. */
    if (env->PushLocalFrame(64) != JNI_OK)
    {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    do
    {
        jobject mc = env->CallStaticObjectMethod(g_cache.mc_class, g_cache.mc_getInstance);

        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (mc == nullptr) break;

        jobject self = env->GetObjectField(mc, g_cache.mc_player);
        jobject world = env->GetObjectField(mc, g_cache.mc_world);

        if (self == nullptr || world == nullptr) break;

        const float progress = tick_progress(env, mc);

        double sx = 0.0, sy = 0.0, sz = 0.0;

        if (!render_pos(env, self, progress, sx, sy, sz)) break;

        const float yaw = g_cache.ent_getYawLerped != nullptr
            ? env->CallFloatMethod(self, g_cache.ent_getYawLerped, progress)
            : env->CallFloatMethod(self, g_cache.ent_getYaw);

        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }

        g_yaw = yaw;

        jobject players = env->CallObjectMethod(world, g_cache.world_getPlayers);

        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (players == nullptr) break;

        const jint count = env->CallIntMethod(players, g_cache.list_size);

        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }

        for (jint i = 0; i < count; ++i)
        {
            jobject other = env->CallObjectMethod(players, g_cache.list_get, i);

            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (other == nullptr) continue;

            /* You are the centre of the dial, not a blip on it. */
            if (env->IsSameObject(other, self) == JNI_TRUE)
            {
                env->DeleteLocalRef(other);
                continue;
            }

            if (g_cache.ent_isAlive != nullptr
                && env->CallBooleanMethod(other, g_cache.ent_isAlive) != JNI_TRUE)
            {
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(other);
                continue;
            }

            if (env->ExceptionCheck()) env->ExceptionClear();

            double ox = 0.0, oy = 0.0, oz = 0.0;

            const bool got = render_pos(env, other, progress, ox, oy, oz);

            env->DeleteLocalRef(other);

            if (!got)
                continue;

            const double dx = ox - sx;
            const double dy = oy - sy;
            const double dz = oz - sz;

            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > static_cast<double>(range))
                continue;

            /* Breeze's bearing, unchanged: -atan2(dx, dz) in degrees is exactly
               Minecraft's yaw convention (0 south, +90 west), so subtracting the
               viewer's yaw leaves an angle measured from straight ahead. */
            const double bearing = -(std::atan2(dx, dz) * (180.0 / k_pi));

            blip b{};
            b.bearing = wrap_degrees(bearing - static_cast<double>(yaw));
            b.distance = static_cast<float>(distance);
            b.ground = static_cast<float>(std::sqrt(dx * dx + dz * dz));
            b.height = static_cast<float>(dy);

            g_blips.push_back(b);
        }
    }
    while (false);

    env->PopLocalFrame(nullptr);

    /* Nearest first, so the cap keeps the players that matter and the draw pass
       can paint far blips under near ones without a second sort. */
    std::sort(g_blips.begin(), g_blips.end(),
        [](const blip& a, const blip& b) { return a.distance < b.distance; });

    if (g_blips.size() > k_max_blips)
        g_blips.resize(k_max_blips);
}

const std::vector<blip>& latest()
{
    return g_blips;
}

float viewer_yaw()
{
    return g_yaw;
}

void reset_source()
{
    g_blips.clear();
    g_yaw = 0.f;
}

} // namespace vanta::modules::radar
