// FastPot — throws splash potions at the rate your hand can click.
//
// MinecraftClient keeps a right-click delay (itemUseCooldown in Yarn,
// rightClickDelay in Mojang) that it seeds with 4 after every item use, and
// while it is above zero handleInputEvents refuses to call doItemUse() even
// with the use key held. That is what caps potion throwing at five a second.
// Clearing it lets every click through.
//
// Nothing here reaches the wire. The field is client-only: the server has no
// such counter, it is never serialised, and zeroing it adds no packet and
// reorders none. What leaves is the same use-item interaction vanilla would
// have sent, on the tick the player asked for it.
//
// SCOPED TO POTIONS, on purpose. Clearing that counter unconditionally does not
// make a FastPot -- it makes a FastUse, and quietly speeds up eating, bows,
// shields and block placement too. So the held item is checked first, and if it
// cannot be identified on this host the module stays inert rather than becoming
// something the player did not switch on. A module should not change what it is
// because a mapping went missing.
//
// Runs from GameLoopEvent, throttled to the world-tick rate -- the pump that in
// MC 1.21 is the game-loop thread, so no bytecode is transformed anywhere.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>

#include <cstring>
#include <jni.h>

namespace vanta::modules {

namespace {

constexpr float TICK_MS = 50.f;

// Vanilla seeds the delay with 4. Anything outside a small range is not this
// counter, so it is left alone rather than clobbered.
constexpr jint PLAUSIBLE_MAX = 40;

struct Cache {
    bool      tried = false;
    bool      ready = false;

    jclass    mc_class        = nullptr;
    jfieldID  mc_player       = nullptr;
    jfieldID  use_cooldown    = nullptr;

    // Held-item chain, for keeping this a FastPot rather than a FastUse.
    jfieldID  player_inv      = nullptr;
    jfieldID  inv_selected    = nullptr;
    jmethodID inv_get_stack   = nullptr;
    jmethodID stack_is_empty  = nullptr;
    jmethodID stack_get_item  = nullptr;
    jmethodID item_translation= nullptr;
};

Cache g_c;

jfieldID field_either(JNIEnv* env, jclass cls, const char* yarn,
                      const char* mojang, const char* sig)
{
    if (!cls) return nullptr;
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    for (const char* n : { mojang_first ? mojang : yarn, mojang_first ? yarn : mojang }) {
        jfieldID f = env->GetFieldID(cls, n, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (f) return f;
    }
    return nullptr;
}

jmethodID method_either(JNIEnv* env, jclass cls, const char* yarn,
                        const char* mojang, const char* sig)
{
    if (!cls) return nullptr;
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    for (const char* n : { mojang_first ? mojang : yarn, mojang_first ? yarn : mojang }) {
        jmethodID m = env->GetMethodID(cls, n, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (m) return m;
    }
    return nullptr;
}

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

    // mappings.hpp already pins this one and warns which neighbour is NOT it,
    // so the documented name leads and the layouts follow.
    for (const char* n : { sdk::mappings::item_use_cooldown_name,
                           "field_1752", "rightClickDelay" }) {
        g_c.use_cooldown = env->GetFieldID(g_c.mc_class, n, "I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (g_c.use_cooldown) break;
    }

    // Held-item chain.
    if (jclass pe = vanta::jvm::findClassAnyLoader(env, sdk::mappings::player_entity_class_sig)) {
        g_c.player_inv = field_either(env, pe, "field_7514", "inventory",
                                      "Lnet/minecraft/class_1661;");
        env->DeleteLocalRef(pe);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass inv = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1661")) {
        g_c.inv_selected  = field_either(env, inv, "field_7545", "selected", "I");
        g_c.inv_get_stack = method_either(env, inv, "method_5438", "getItem",
                                          "(I)Lnet/minecraft/class_1799;");
        env->DeleteLocalRef(inv);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass st = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1799")) {
        g_c.stack_is_empty = method_either(env, st, "method_7960", "isEmpty", "()Z");
        g_c.stack_get_item = method_either(env, st, "method_7909", "getItem",
                                           "()Lnet/minecraft/class_1792;");
        env->DeleteLocalRef(st);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass it = vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1792")) {
        g_c.item_translation = method_either(env, it, "method_7876", "getDescriptionId",
                                             "()Ljava/lang/String;");
        env->DeleteLocalRef(it);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    const bool item_chain = g_c.player_inv && g_c.inv_selected && g_c.inv_get_stack
                         && g_c.stack_is_empty && g_c.stack_get_item && g_c.item_translation;

    g_c.ready = g_c.mc_player && g_c.use_cooldown && item_chain;

    VLOG_INFO("fastpot", "cooldown={} held-item chain={} -> {}",
              g_c.use_cooldown ? 1 : 0, item_chain ? 1 : 0,
              g_c.ready ? "ready"
                        : "inert (refusing to speed up every item, not just potions)");
}

// Matched by translation key rather than item identity: one string test covers
// splash and lingering, every colour and every effect, on any version.
bool holding_throwable_potion(JNIEnv* env, jobject player)
{
    jobject inv = env->GetObjectField(player, g_c.player_inv);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!inv) return false;

    const jint slot = env->GetIntField(inv, g_c.inv_selected);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(inv); return false; }

    jobject stack = env->CallObjectMethod(inv, g_c.inv_get_stack, slot);
    env->DeleteLocalRef(inv);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!stack) return false;

    bool result = false;
    const jboolean empty = env->CallBooleanMethod(stack, g_c.stack_is_empty);
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (empty != JNI_TRUE) {
        jobject item = env->CallObjectMethod(stack, g_c.stack_get_item);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (item) {
            jstring key = static_cast<jstring>(
                env->CallObjectMethod(item, g_c.item_translation));
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (key) {
                const char* k = env->GetStringUTFChars(key, nullptr);
                if (k) {
                    result = std::strstr(k, "splash_potion")
                          || std::strstr(k, "lingering_potion");
                    env->ReleaseStringUTFChars(key, k);
                }
                env->DeleteLocalRef(key);
            }
            env->DeleteLocalRef(item);
        }
    }
    env->DeleteLocalRef(stack);
    return result;
}

} // namespace

class FastPot : public Module {
public:
    FastPot() : Module("FastPot",
                       "Throws pots as fast as you click",
                       Category::Combat) {
        registerSetting(&speed_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&){ pump(); });
    }

private:
    // 1 is vanilla, 5 is as fast as one interaction per tick allows.
    //
    // The number is the ceiling the right-click delay is allowed to keep:
    // 5 - speed. Vanilla seeds 4 and counts down, firing at zero, so the
    // ceiling IS the period.
    //
    //   speed 1 -> 4 ->  4 throws/s   (untouched)
    //   speed 2 -> 3 ->  5/s
    //   speed 3 -> 2 -> ~6.7/s
    //   speed 4 -> 1 -> 10/s
    //   speed 5 -> 0 -> 20/s
    //
    // Twenty a second is the structural ceiling, not a chosen limit:
    // handleInputEvents runs once per tick and calls doItemUse at most once
    // from the held-key branch. Going past it means pushing extra presses into
    // the key's queue so several fire in ONE tick -- several use-item packets
    // on the same tick, which is exactly the shape the MultiActions family
    // looks for. That line is not crossed silently.
    IntSetting speed_ {this, "Speed", 5, 1, 5};

    void pump()
    {
        if (!isEnabled()) return;

        // Deliberately NOT throttled to the tick rate.
        //
        // The previous version ran on a free-running 50 ms accumulator, which
        // is the same period as the world tick but not phase-locked to it. Some
        // tick intervals got no pump at all, so the delay was still above zero
        // when handleInputEvents looked, and that throw was simply lost. Running
        // every frame guarantees the field is clear whenever the tick reads it,
        // which is most of the speed-up on its own.
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

        // Read the counter FIRST, and only walk the inventory when there is
        // actually something to clamp. Running every frame instead of every
        // tick multiplies this work by the frame rate, and on the vast majority
        // of frames the answer is "nothing to do" after a single field read.
        const jint max_cd = static_cast<jint>(5 - speed_.value());
        const jint now    = env->GetIntField(mc, g_c.use_cooldown);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }

        if (now <= max_cd || now > PLAUSIBLE_MAX) { env->DeleteLocalRef(mc); return; }

        jobject player = env->GetObjectField(mc, g_c.mc_player);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }

        if (player) {
            if (holding_throwable_potion(env, player)) {
                // A CLAMP, never a pin. The strict "greater than" above is what
                // makes the difference: writing the ceiling unconditionally on
                // every frame would hold the counter at that value forever, it
                // would never reach zero, and the module would BLOCK potions
                // instead of speeding them up. Clamping once lets vanilla count
                // the rest of the way down on its own.
                env->SetIntField(mc, g_c.use_cooldown, max_cd);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(player);
        }
        env->DeleteLocalRef(mc);
    }
};

VANTA_REGISTER_MODULE(FastPot);

} // namespace vanta::modules
