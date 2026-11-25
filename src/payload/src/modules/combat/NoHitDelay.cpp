// NoHitDelay — removes the client-side gate that swallows attacks.
//
// MinecraftClient keeps a small countdown (attackCooldown / missTime) that
// doAttack() refuses to run while it is above zero. It exists to stop a single
// held click from registering as a burst, and its side effect is that an attack
// made shortly after breaking a block, or after a missed swing, is discarded
// before it ever becomes a packet. Zeroing it each tick means every click the
// player actually makes reaches doAttack().
//
// Nothing here touches the wire. The field is client-only state: it does not
// exist on the server, it is never serialised, and clearing it adds no packet
// and reorders none. What leaves the client is the same attack it would have
// sent anyway, on the tick the player asked for it -- so there is nothing for a
// movement or combat check to see beyond a click rate, and the click rate is
// still whatever the player's hand produces.
//
// This is emphatically NOT the 1.9 attack-strength charge. That one lives on
// PlayerEntity, the server computes it independently, and forcing it client
// side would only produce a stream of weakened hits. This is the input gate,
// not the damage curve.
//
// Runs from GameLoopEvent, throttled to the world-tick rate -- the same pump
// AutoTotem and SilentAim use, which in MC 1.21 is the game-loop thread.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>
#include <jni.h>

namespace dt::modules {

namespace {

constexpr float TICK_MS = 50.f;

// A cooldown that plausibly belongs to this field is a small countdown. If the
// name resolves to something else entirely on some host, its value will not sit
// in this range and it is left alone rather than clobbered.
constexpr jint PLAUSIBLE_MAX = 40;

struct Cache {
    bool     tried    = false;
    bool     ready    = false;
    jclass   mc_class = nullptr;
    jfieldID cooldown = nullptr;
};

Cache g_c;

void build_cache(JNIEnv* env)
{
    if (g_c.tried) return;
    g_c.tried = true;

    jclass local = dt::jvm::findClassAnyLoader(env, sdk::mappings::minecraftclass_sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    g_c.mc_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_c.mc_class) return;

    // Both layouts, because bind_mappings() is not wired in this tree and a
    // Mojang-mapped host would otherwise resolve nothing.
    const char* names[] = { sdk::mappings::attack_cooldown_name, "field_1771", "missTime" };
    for (const char* n : names) {
        g_c.cooldown = env->GetFieldID(g_c.mc_class, n, "I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (g_c.cooldown) break;
    }

    g_c.ready = g_c.cooldown != nullptr;
    VLOG_INFO("nohitdelay", "attack cooldown field: {}",
              g_c.ready ? "resolved" : "NOT FOUND - module inert");
}

} // namespace

class NoHitDelay : public Module {
public:
    NoHitDelay() : Module("NoHitDelay",
                          "Kills the delay that eats your hits",
                          Category::Combat) {}

    ~NoHitDelay() override { releaseCache(); }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){ pump(e.deltaMs); });
    }

private:
    void onDisable() override { releaseCache(); }
    static void releaseCache() {
        dt::jvm::JniScope scope;
        if (!scope) return;
        if (g_c.mc_class) scope.get()->DeleteGlobalRef(g_c.mc_class);
        g_c = {};
    }
    void pump(float delta_ms)
    {
        if (!isEnabled()) return;

        static double accum = 0.0;
        accum += (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.0;
        if (accum < TICK_MS) return;
        accum -= TICK_MS;
        if (accum > TICK_MS) accum = TICK_MS;

        dt::jvm::JniScope scope;
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

        const jint now = env->GetIntField(mc, g_c.cooldown);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (now > 0 && now <= PLAUSIBLE_MAX) {
            env->SetIntField(mc, g_c.cooldown, 0);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(mc);
    }
};

DT_REGISTER_MODULE(NoHitDelay);

} // namespace dt::modules
