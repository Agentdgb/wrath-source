// AutoJumpReset — jumps when you take a hit, to reset knockback.
//
// The whole design rests on one decision: the jump is produced by pressing
// VANILLA'S OWN jump keybinding for a tick, never by calling jump() or by
// writing velocity.
//
// That matters because it makes the module undetectable by construction rather
// than by disguise. Input.tick() reads the binding, ClientPlayerEntity applies
// every vanilla precondition (on ground, jump cooldown, sneaking, vehicles,
// fluids), and sendMovementPackets emits the result. The packets that leave are
// byte-for-byte what a real key press produces, in the same order, from the
// same call sites. There is nothing extra on the wire for a movement or
// prediction check to notice — Grim replays a perfectly ordinary jump because
// it IS a perfectly ordinary jump.
//
// What remains detectable is not the jump but the REACTION: a bot that jumps
// on every single hit, always the same number of ticks later, is a pattern no
// hand produces. So the two things that are randomised are exactly those — the
// chance slider decides whether to react at all, and the delay is re-drawn per
// hit. A constant is the signature; this whole client has learned that lesson
// the hard way.
//
// Runs from GameLoopEvent, throttled to the world-tick rate. In MC 1.21 that
// pump is the game-loop thread, so the JNI work lands on the game thread with
// no bytecode transform anywhere — same path AutoTotem and SilentAim use.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"   // isOverlayVisible — stand down while the panel is up

#include <sdk/mappings/mappings.hpp>

#include <cstring>
#include <jni.h>
#include <random>

namespace dt::modules {

namespace {

constexpr float TICK_MS = 50.f;

// ── who is allowed to trigger a reset ────────────────────────────────────────
enum class Source : int { Players = 0, Mobs = 1, All = 2 };

// ── JNI cache ────────────────────────────────────────────────────────────────
struct Cache {
    bool      ready = false;
    bool      tried = false;

    jclass    minecraft_class      = nullptr;
    jfieldID  mc_player_fid        = nullptr;
    jfieldID  mc_options_fid       = nullptr;
    jfieldID  mc_screen_fid        = nullptr;

    jclass    entity_class         = nullptr;
    jmethodID entity_on_ground_mid = nullptr;

    jclass    living_class         = nullptr;
    jfieldID  living_hurt_time_fid = nullptr;
    jmethodID living_attacker_mid  = nullptr;   // optional; null → no filtering

    jclass    player_class         = nullptr;

    // Resolved lazily once a live GameOptions exists.
    jfieldID  jump_key_fid         = nullptr;
    jfieldID  key_pressed_fid      = nullptr;
    bool      keys_resolved        = false;
};

Cache g_c;

std::mt19937& rng()
{
    static std::mt19937 g(std::random_device{}());
    return g;
}

int rand_int(int lo, int hi)
{
    std::uniform_int_distribution<int> d(lo, hi);
    return d(rng());
}

// ── small reflection helpers, same shape the other modules use ───────────────
bool cache_class(JNIEnv* env, const char* sig, jclass& out)
{
    if (!sig) return false;
    jclass local = dt::jvm::findClassAnyLoader(env, sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    out = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return out != nullptr;
}

jfieldID cache_field(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID f = env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return f;
}

jmethodID cache_method(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

// Try the layout the mappings claim first, then the other one. bind_mappings()
// is not wired in this tree, so a Mojang-mapped host would otherwise resolve
// nothing at all.
jfieldID cache_field_either(JNIEnv* env, jclass cls,
                            const char* yarn, const char* mojang, const char* sig)
{
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    jfieldID f = cache_field(env, cls, mojang_first ? mojang : yarn, sig);
    if (!f) f = cache_field(env, cls, mojang_first ? yarn : mojang, sig);
    return f;
}

jmethodID cache_method_either(JNIEnv* env, jclass cls,
                              const char* yarn, const char* mojang, const char* sig)
{
    const bool mojang_first = sdk::mappings::is_mojang_layout;
    jmethodID m = cache_method(env, cls, mojang_first ? mojang : yarn, sig);
    if (!m) m = cache_method(env, cls, mojang_first ? yarn : mojang, sig);
    return m;
}

// KeyBinding.pressed is the only boolean field the class declares, so it can be
// found by SHAPE rather than by a name that moves between 1.21.x drops. Three
// separate name guesses in this client turned out wrong before this trick was
// adopted; identifying a field by something structural cannot be wrong that way.
jfieldID find_sole_boolean_field(JNIEnv* env, jclass cls)
{
    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!class_cls || !field_cls) return nullptr;

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    jmethodID get_type   = env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;");
    jmethodID get_mods   = env->GetMethodID(field_cls, "getModifiers", "()I");
    jmethodID type_name  = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!get_fields || !get_name || !get_type || !get_mods || !type_name) return nullptr;

    jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(cls, get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!arr) return nullptr;

    jfieldID found = nullptr;
    int      hits  = 0;
    const jsize n = env->GetArrayLength(arr);
    for (jsize k = 0; k < n; ++k) {
        jobject fld = env->GetObjectArrayElement(arr, k);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!fld) continue;

        const jint mods = env->CallIntMethod(fld, get_mods);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        if (mods & 0x0008) { env->DeleteLocalRef(fld); continue; }   // ACC_STATIC

        jobject ty = env->CallObjectMethod(fld, get_type);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        if (!ty) { env->DeleteLocalRef(fld); continue; }

        jstring tname = static_cast<jstring>(env->CallObjectMethod(ty, type_name));
        env->DeleteLocalRef(ty);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }

        bool is_bool = false;
        if (tname) {
            const char* tn = env->GetStringUTFChars(tname, nullptr);
            if (tn) { is_bool = (std::strcmp(tn, "boolean") == 0);
                      env->ReleaseStringUTFChars(tname, tn); }
            env->DeleteLocalRef(tname);
        }
        if (!is_bool) { env->DeleteLocalRef(fld); continue; }

        jstring jname = static_cast<jstring>(env->CallObjectMethod(fld, get_name));
        env->DeleteLocalRef(fld);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!jname) continue;

        const char* nm = env->GetStringUTFChars(jname, nullptr);
        if (nm) {
            jfieldID fid = env->GetFieldID(cls, nm, "Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (fid) { found = fid; ++hits; }
            env->ReleaseStringUTFChars(jname, nm);
        }
        env->DeleteLocalRef(jname);
    }
    env->DeleteLocalRef(arr);

    return (hits == 1) ? found : nullptr;   // exactly one, or we have not identified it
}

void build_cache(JNIEnv* env)
{
    if (g_c.tried) return;
    g_c.tried = true;

    if (!cache_class(env, sdk::mappings::minecraftclass_sig, g_c.minecraft_class)) return;

    g_c.mc_player_fid  = cache_field(env, g_c.minecraft_class,
        sdk::mappings::player_name, sdk::mappings::player_sig);
    g_c.mc_options_fid = cache_field_either(env, g_c.minecraft_class,
        "field_1690", "options", "Lnet/minecraft/class_315;");
    g_c.mc_screen_fid  = cache_field(env, g_c.minecraft_class,
        "field_1755", "Lnet/minecraft/class_437;");

    cache_class(env, sdk::mappings::entity_class_sig,        g_c.entity_class);
    cache_class(env, "net/minecraft/class_1309",             g_c.living_class);
    cache_class(env, sdk::mappings::player_entity_class_sig, g_c.player_class);

    if (g_c.entity_class) {
        g_c.entity_on_ground_mid = cache_method(env, g_c.entity_class,
            sdk::mappings::is_on_ground_name, sdk::mappings::is_on_ground_sig);
    }

    if (g_c.living_class) {
        g_c.living_hurt_time_fid = cache_field(env, g_c.living_class,
            sdk::mappings::living_entity_hurt_time_name,
            sdk::mappings::living_entity_hurt_time_sig);

        // Optional. Without it the Source filter cannot be honoured, so the
        // module falls back to reacting to every hit rather than to none --
        // a missing mapping should cost precision, never function.
        g_c.living_attacker_mid = cache_method_either(env, g_c.living_class,
            "method_6065", "getLastHurtByMob", "()Lnet/minecraft/class_1309;");
    }

    g_c.ready = g_c.minecraft_class && g_c.mc_player_fid && g_c.living_hurt_time_fid;

    VLOG_INFO("autojumpreset",
              "cache: hurtTime={} onGround={} attacker={} options={} -> {}",
              g_c.living_hurt_time_fid ? 1 : 0,
              g_c.entity_on_ground_mid ? 1 : 0,
              g_c.living_attacker_mid  ? 1 : 0,
              g_c.mc_options_fid       ? 1 : 0,
              g_c.ready ? "ready" : "unusable");
}

// Finds the jump binding on GameOptions by its TRANSLATION KEY, which is stable
// across every version and every mapping flavour, instead of by a field name
// that is not.
void resolve_jump_key(JNIEnv* env, jobject mc)
{
    if (g_c.keys_resolved || !g_c.mc_options_fid) return;

    jobject options = env->GetObjectField(mc, g_c.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;

    jclass kb_cls = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_304");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID kb_translation = kb_cls
        ? cache_method_either(env, kb_cls, "method_1431", "getName", "()Ljava/lang/String;")
        : nullptr;
    if (kb_cls && !g_c.key_pressed_fid)
        g_c.key_pressed_fid = find_sole_boolean_field(env, kb_cls);

    jclass opts_cls  = env->GetObjectClass(options);
    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

    jobjectArray arr = get_fields
        ? static_cast<jobjectArray>(env->CallObjectMethod(opts_cls, get_fields)) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); arr = nullptr; }

    if (arr && kb_translation) {
        const jsize n = env->GetArrayLength(arr);
        for (jsize k = 0; k < n && !g_c.jump_key_fid; ++k) {
            jobject fld = env->GetObjectArrayElement(arr, k);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!fld) continue;

            jstring jname = static_cast<jstring>(env->CallObjectMethod(fld, get_name));
            env->DeleteLocalRef(fld);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!jname) continue;

            const char* nm = env->GetStringUTFChars(jname, nullptr);
            jfieldID fid = nullptr;
            if (nm) {
                fid = env->GetFieldID(opts_cls, nm, "Lnet/minecraft/class_304;");
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->ReleaseStringUTFChars(jname, nm);
            }
            env->DeleteLocalRef(jname);
            if (!fid) continue;

            jobject kb = env->GetObjectField(options, fid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!kb) continue;

            jstring tk = static_cast<jstring>(env->CallObjectMethod(kb, kb_translation));
            env->DeleteLocalRef(kb);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!tk) continue;

            const char* t = env->GetStringUTFChars(tk, nullptr);
            if (t && std::strcmp(t, "key.jump") == 0) g_c.jump_key_fid = fid;
            if (t) env->ReleaseStringUTFChars(tk, t);
            env->DeleteLocalRef(tk);
        }
    }
    if (arr) env->DeleteLocalRef(arr);
    env->DeleteLocalRef(options);

    if (g_c.jump_key_fid && g_c.key_pressed_fid) {
        g_c.keys_resolved = true;
        VLOG_INFO("autojumpreset", "jump binding resolved by translation key");
    }
}

} // namespace

class AutoJumpReset : public Module {
public:
    AutoJumpReset() : Module("AutoJumpReset",
                             "Jumps on hit to reset knockback",
                             Category::Movement) {
        registerSetting(&chance_);
        registerSetting(&source_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){ pump(e.deltaMs); });
    }

    void onDisable() override {
        // The release is left to the pump: it owns the binding and runs on the
        // game thread, while this runs on the render thread. Dropping the key
        // from here would be a cross-thread write to live game state.
        armed_ticks_ = -1;
    }

private:
    // Vape-style: a percentage, not a boolean. Reacting to every single hit is
    // the tell -- a human misses plenty of them.
    IntSetting  chance_ {this, "Chance", 50, 0, 100};

    EnumSetting source_ {this, "Source", {"Players", "Mobs", "All"}, 2};

    // -1 idle, otherwise ticks remaining before firing.
    int  armed_ticks_ = -1;
    // Ticks left holding the binding down once fired.
    int  hold_ticks_  = 0;
    int  last_hurt_   = 0;
    bool key_held_    = false;   // we are the ones holding it

    void pump(float delta_ms)
    {
        // One step per world tick. The reaction delay below is counted in
        // ticks, so it has to be driven at the tick rate to mean anything.
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

        jfieldID inst = env->GetStaticFieldID(g_c.minecraft_class,
            sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!inst) return;

        jobject mc = env->GetStaticObjectField(g_c.minecraft_class, inst);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!mc) return;

        resolve_jump_key(env, mc);

        jobject player = env->GetObjectField(mc, g_c.mc_player_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }

        if (player) {
            step(env, mc, player);
            env->DeleteLocalRef(player);
        } else {
            armed_ticks_ = -1;
            last_hurt_   = 0;
        }
        env->DeleteLocalRef(mc);
    }

    void step(JNIEnv* env, jobject mc, jobject player)
    {
        // A key we pressed is held for a COUNTED number of ticks, then always
        // released -- and the release runs ahead of every early return below so
        // the binding can never be left latched. The client already had one bug
        // where a swallowed release left the attack key stuck down; that is not
        // a shape worth repeating.
        //
        // It is counted rather than released on the next pump because the two
        // clocks do not line up. This pump is a free-running 50 ms accumulator
        // and the world tick is its own 50 ms clock, so pressing on one pump and
        // releasing on the next leaves a real chance that NO tick ran in
        // between and Input.tick() never saw the key at all. Holding across two
        // to four pumps guarantees at least one tick reads it -- and 100-200 ms
        // is what a hand does with a jump key anyway.
        if (key_held_ && --hold_ticks_ <= 0) release_jump(env, mc);

        if (!isEnabled()) { armed_ticks_ = -1; release_jump(env, mc); return; }

        // Stand down while the ClickGui is up. The panel is an ImGui surface,
        // not a Minecraft Screen, so currentScreen is null while it is open and
        // a null check does not see it.
        if (::dt::vgui::bridge::isOverlayVisible()) {
            armed_ticks_ = -1; release_jump(env, mc); return;
        }

        if (g_c.mc_screen_fid) {
            jobject screen = env->GetObjectField(mc, g_c.mc_screen_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (screen) {
                env->DeleteLocalRef(screen);
                armed_ticks_ = -1; release_jump(env, mc); return;
            }
        }

        const int hurt = env->GetIntField(player, g_c.living_hurt_time_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

        // Rising edge. hurtTime is set to its maximum the moment damage lands
        // and counts down from there, so a jump from zero is exactly one hit --
        // no packet hook and no event needed, which is what keeps this working
        // on every version and launcher.
        const bool just_hit = (hurt > last_hurt_) && (last_hurt_ == 0);
        last_hurt_ = hurt;

        if (just_hit && armed_ticks_ < 0 && !key_held_ && passes_source(env, player)
            && rand_int(1, 100) <= chance_.value()) {
            // Zero or one tick, and no more.
            //
            // The first version drew 1-3 ticks here, reasoning that a varied
            // reaction time is less of a pattern. It is -- but it also misses
            // the window. Knockback lifts you off the ground almost at once, so
            // by tick three the onGround gate below was silently throwing the
            // jump away, and the module only appeared to work while sprinting
            // forward, where you stay planted longer. A jump reset has to land
            // while you are still on the ground or it is not a reset at all.
            //
            // The variation lives in the chance roll instead, which is where it
            // belongs: a human misses hits, they do not react to them late.
            armed_ticks_ = rand_int(0, 1);
        }

        if (armed_ticks_ < 0) return;
        if (armed_ticks_-- > 0) return;

        // Only from the ground. Vanilla would ignore the press mid-air anyway,
        // so this changes nothing the server sees -- it just avoids holding a
        // binding down to no purpose.
        if (g_c.entity_on_ground_mid) {
            const jboolean og = env->CallBooleanMethod(player, g_c.entity_on_ground_mid);
            if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
            if (og != JNI_TRUE) return;
        }

        press_jump(env, mc);
    }

    // Players / Mobs / All. If the attacker cannot be read on this host the
    // filter opens rather than closing: a missing mapping should cost
    // precision, never function.
    bool passes_source(JNIEnv* env, jobject player)
    {
        const auto want = static_cast<Source>(source_.index());
        if (want == Source::All) return true;
        if (!g_c.living_attacker_mid || !g_c.player_class) return true;

        jobject attacker = env->CallObjectMethod(player, g_c.living_attacker_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
        if (!attacker) return true;

        const bool is_player =
            env->IsInstanceOf(attacker, g_c.player_class) == JNI_TRUE;
        env->DeleteLocalRef(attacker);

        return want == Source::Players ? is_player : !is_player;
    }

    // Press vanilla's own binding. Input.tick() reads it later in the same
    // tick, so the jump is produced by the game's own code path and the wire
    // carries nothing a real key press would not.
    void press_jump(JNIEnv* env, jobject mc)
    {
        if (!g_c.keys_resolved) return;

        jobject options = env->GetObjectField(mc, g_c.mc_options_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!options) return;

        jobject kb = env->GetObjectField(options, g_c.jump_key_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

        if (kb) {
            const jboolean already = env->GetBooleanField(kb, g_c.key_pressed_fid);
            if (env->ExceptionCheck()) env->ExceptionClear();
            // The player is already holding jump: leave it entirely alone.
            // Writing true and then false would RELEASE a key they are still
            // holding, which is worse than doing nothing.
            if (already != JNI_TRUE) {
                env->SetBooleanField(kb, g_c.key_pressed_fid, JNI_TRUE);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else { key_held_ = true; hold_ticks_ = rand_int(2, 4); }
            }
            env->DeleteLocalRef(kb);
        }
        env->DeleteLocalRef(options);
    }

    void release_jump(JNIEnv* env, jobject mc)
    {
        if (!key_held_) return;
        key_held_   = false;
        hold_ticks_ = 0;
        if (!g_c.keys_resolved) return;

        jobject options = env->GetObjectField(mc, g_c.mc_options_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!options) return;

        jobject kb = env->GetObjectField(options, g_c.jump_key_fid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }
        if (kb) {
            env->SetBooleanField(kb, g_c.key_pressed_fid, JNI_FALSE);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(kb);
        }
        env->DeleteLocalRef(options);
    }
};

DT_REGISTER_MODULE(AutoJumpReset);

} // namespace dt::modules
