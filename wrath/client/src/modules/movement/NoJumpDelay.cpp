// NoJumpDelay — clears the cooldown that blocks a second jump.
//
// LivingEntity sets jumpingCooldown (Mojang calls it noJumpDelay, which is
// where the module gets its name) to 10 the moment you jump, and refuses
// another jump until it counts back down. Its purpose is to stop a held jump
// key from firing repeatedly. Clearing it each tick means landing is the only
// thing that gates the next jump, so a held key bunny-hops at the rate the
// ground allows.
//
// A note on where this actually changes anything: on flat ground a normal jump
// keeps you airborne longer than the ten ticks, so the cooldown has already
// expired by the time you land and this does nothing at all. It only bites
// where you land early -- down a slope, off stairs, in a low ceiling.
//
// And a note on risk, because it is not the same as the other movement work in
// this client. The jump itself is still vanilla's: this only clears a gate, and
// LivingEntity.jump() runs from its own call site with every other condition
// intact, so no packet is added, removed or reordered. But a server-side
// movement simulation that models this cooldown would predict no jump on the
// tick one happens. If Simulation flags start appearing on landings, this is
// the first module to switch off -- that is an honest trade, not a hidden one.
//
// The field is confirmed by BEHAVIOUR before it is ever written. Three separate
// intermediary-name guesses in this client turned out to be wrong fields, and
// writing into one of those is how the held item ended up flying off screen. A
// countdown that steps down by exactly one per tick from a value no greater
// than ten is a signature nothing else on LivingEntity shares; until it is
// seen, this module reads and writes nothing.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>
#include <jni.h>

namespace vanta::modules {

namespace {

constexpr float TICK_MS = 50.f;

// Vanilla seeds the cooldown at exactly 10.
constexpr jint COOLDOWN_START = 10;

struct Cache {
    bool     tried        = false;
    bool     ready        = false;
    jclass   mc_class     = nullptr;
    jfieldID mc_player    = nullptr;
    jclass   living_class = nullptr;
    jfieldID cooldown     = nullptr;
};

Cache g_c;

void build_cache(JNIEnv* env)
{
    if (g_c.tried) return;
    g_c.tried = true;

    jclass local = vanta::jvm::findClassAnyLoader(env, sdk::mappings::minecraftclass_sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    g_c.mc_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_c.mc_class) return;

    g_c.mc_player = env->GetFieldID(g_c.mc_class,
        sdk::mappings::player_name, sdk::mappings::player_sig);
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass lv = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1309");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (lv) {
        g_c.living_class = static_cast<jclass>(env->NewGlobalRef(lv));
        env->DeleteLocalRef(lv);
    }

    if (g_c.living_class) {
        // Both layouts, because bind_mappings() is not wired in this tree.
        const char* names[] = {
            sdk::mappings::living_entity_jumping_cooldown_name,
            "field_6228",
            "noJumpDelay",
        };
        for (const char* n : names) {
            g_c.cooldown = env->GetFieldID(g_c.living_class, n, "I");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (g_c.cooldown) break;
        }
    }

    g_c.ready = g_c.mc_player && g_c.cooldown;
    VLOG_INFO("nojumpdelay", "jump cooldown field: {}",
              g_c.ready ? "resolved - confirming by behaviour before use"
                        : "NOT FOUND - module inert");
}

} // namespace

class NoJumpDelay : public Module {
public:
    NoJumpDelay() : Module("Jump Assist",
                           "Jump again with no cooldown",
                           Category::Movement) {}

    ~NoJumpDelay() override { releaseCache(); }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){ pump(e.deltaMs); });
    }

private:
    void onDisable() override { releaseCache(); prev_ = -1; confirmed_ = false; }
    static void releaseCache() {
        vanta::jvm::JniScope scope;
        if (!scope) return;
        if (g_c.mc_class) scope.get()->DeleteGlobalRef(g_c.mc_class);
        if (g_c.living_class) scope.get()->DeleteGlobalRef(g_c.living_class);
        g_c = {};
    }
    jint prev_      = -1;
    bool confirmed_ = false;

    void pump(float delta_ms)
    {
        if (!isEnabled()) return;

        static double accum = 0.0;
        accum += (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.0;
        if (accum < TICK_MS) return;
        accum -= TICK_MS;
        if (accum > TICK_MS) accum = TICK_MS;

        vanta::jvm::JniScope scope;
        JNIEnv* env = scope.get();
        if (!env) return;

        build_cache(env);
        if (!g_c.ready) return;

        jfieldID inst = env->GetStaticFieldID(g_c.mc_class,
            sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!inst) return;

        jobject mc = env->GetStaticObjectField(g_c.mc_class, inst);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!mc) return;

        jobject player = env->GetObjectField(mc, g_c.mc_player);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }
        env->DeleteLocalRef(mc);
        if (!player) { prev_ = -1; return; }

        step(env, player);
        env->DeleteLocalRef(player);
    }

    void step(JNIEnv* env, jobject player)
    {
        const jint now = env->GetIntField(player, g_c.cooldown);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

        // Confirm by behaviour before ever writing. A field that steps down by
        // exactly one per tick, from no more than the ten vanilla seeds it
        // with, is a countdown -- and on LivingEntity that is this one. Until
        // that is observed the module only watches.
        if (!confirmed_) {
            if (prev_ > 0 && prev_ <= COOLDOWN_START && now == prev_ - 1) {
                confirmed_ = true;
                VLOG_INFO("nojumpdelay",
                          "jump cooldown confirmed by countdown signature - now active");
            }
            prev_ = now;
            return;
        }

        prev_ = now;
        if (now > 0 && now <= COOLDOWN_START) {
            env->SetIntField(player, g_c.cooldown, 0);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
};

VANTA_REGISTER_MODULE(NoJumpDelay);

} // namespace vanta::modules
