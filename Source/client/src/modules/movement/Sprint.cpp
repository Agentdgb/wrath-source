// Sprint — holds the sprint key down for you, so you always sprint and start
// swimming the moment you go under.
//
// It holds the KEY. It does not set the sprint state, and the difference is the
// entire module.
//
// The first version called Entity.setSprinting(true) directly and argued that
// vanilla's own stop test in the next tickMovement would validate it. That
// argument was sound as far as it went -- a bad write really is erased before
// any packet is built -- but it missed where the server actually gets its
// information. Since 1.21.2 the client reports its raw input in a PlayerInput
// packet, and Grim predicts movement from that report:
//
//     knownInput = new KnownInput(forward, backward, left, right, jump, shift, sprint)
//
// Setting the entity flag made the player move at sprint speed and sent
// START_SPRINTING, but the sprint bit in PlayerInput stayed false, because no
// key was down to justify it. Grim predicted an unsprinted player, watched a
// sprinting one, and flagged Simulation -- worst in water, where the swim state
// makes the speed gap largest, and in a bubble column, where being pulled
// vertically leaves no slack for the error to hide in.
//
// Pressing the binding removes the whole class of problem rather than patching
// it. Vanilla reads the key, applies its own start conditions -- on ground or
// submerged, not sneaking, forward held, not using an item, food above 6, no
// blindness -- flips the state at the point in the tick it always does, and
// reports the sprint bit in PlayerInput itself. There is no divergence to
// explain because there is nothing left that differs from a player holding the
// key with their finger.
//
// Water then follows for free. Vanilla drops a sprint at the SURFACE (touching
// water, not submerged) and keeps it while submerged, and updateSwimming()
// turns "sprinting while submerged" into the swim pose. A held key means the
// swim resumes the instant the player goes under, and at the surface vanilla
// still drops it, exactly as it would for anyone else.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "modules/shared/movement_idle.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <sdk/mappings/mappings.hpp>

#include <jni.h>

namespace vanta::modules {

namespace {

constexpr float TICK_MS = 50.f;

jclass    g_mc_class      = nullptr;
bool      g_tried         = false;

void build_cache(JNIEnv* env)
{
    if (g_tried) return;
    g_tried = true;

    jclass local = vanta::jvm::findClassAnyLoader(env, sdk::mappings::minecraftclass_sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    g_mc_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);

    VLOG_INFO("sprint", "cache: MinecraftClient {}", g_mc_class ? "resolved" : "NOT FOUND");
}

jobject resolve_minecraft(JNIEnv* env)
{
    if (!g_mc_class) return nullptr;
    jfieldID inst = env->GetStaticFieldID(g_mc_class,
        sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!inst) return nullptr;
    jobject mc = env->GetStaticObjectField(g_mc_class, inst);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return mc;
}

} // namespace

class Sprint : public Module {
public:
    Sprint() : Module("Sprint",
                      "Always sprinting, and swims underwater",
                      Category::Movement) {}

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){ pump(e.deltaMs); });
    }

    // Let go of the key on the way out, or it stays held after the module is
    // switched off and the player sprints forever with nothing driving it.
    void onDisable() override { release(); }

private:
    bool held_ = false;

    void pump(float delta_ms)
    {
        // Once per tick is plenty: the binding only has to be down when
        // KeyboardInput.tick() next samples it.
        static double accum = 0.0;
        accum += (delta_ms > 0.f && delta_ms < 1000.f) ? delta_ms : 0.0;
        if (accum < TICK_MS) return;
        accum -= TICK_MS;
        if (accum > TICK_MS) accum = TICK_MS;

        vanta::jvm::JniScope scope;
        JNIEnv* env = scope.get();
        if (!env) return;

        build_cache(env);
        if (!g_mc_class) return;

        jobject mc = resolve_minecraft(env);
        if (!mc) return;

        // Stand down while the ClickGui owns the input, and while disabled --
        // both have to actively release, not merely stop pressing, because
        // nothing else will ever set the binding back to false.
        //
        // sprint_reset_active() covers the two cases where someone else needs
        // the sprint gone: the reset vanilla performs on an attack, which has
        // to stay visible until sendMovementPackets reports it, and the hold
        // the triggerbot takes out when it is prioritising crits. Both are
        // self-expiring, so this module never has to know why.
        const bool want = isEnabled()
                       && !::vanta::vgui::bridge::isOverlayVisible()
                       && !::vanta::shared::sprint_reset_active();

        if (want) {
            ::vanta::shared::set_movement_key(env, mc, ::vanta::shared::KEY_SPRINT, true);
            held_ = true;
        } else if (held_) {
            ::vanta::shared::set_movement_key(env, mc, ::vanta::shared::KEY_SPRINT, false);
            held_ = false;
        }

        env->DeleteLocalRef(mc);
    }

    // onDisable runs on whatever thread toggled the module, so it takes its own
    // scope rather than borrowing the pump's.
    void release()
    {
        if (!held_) return;
        vanta::jvm::JniScope scope;
        JNIEnv* env = scope.get();
        if (!env) return;
        if (jobject mc = resolve_minecraft(env)) {
            ::vanta::shared::set_movement_key(env, mc, ::vanta::shared::KEY_SPRINT, false);
            env->DeleteLocalRef(mc);
        }
        held_ = false;
    }
};

VANTA_REGISTER_MODULE(Sprint);

} // namespace vanta::modules
