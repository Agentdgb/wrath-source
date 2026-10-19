// Refill — keeps chosen hotbar slots stocked from the main inventory.
//
// Category is Combat rather than Utilities because it restocks combat
// consumables and shares its whole mechanism with AutoTotem: open the player
// screen, issue one real ClickSlot in SWAP mode, close. Putting it next to the
// module it is a sibling of beats filing it by the fact that it touches an
// inventory.
//
// Item identity: every potion shares ONE item and one translation key, so
// healing, swiftness and strength are indistinguishable there. Since 1.20.5 the
// effect is not folded into the stack's translation key either -- it moved into
// the components system and now only surfaces through the stack's NAME. So the
// key is taken in layers, best first:
//
//   1. ItemStack.getName() -> Text -> TranslatableTextContent.getKey()
//      "item.minecraft.splash_potion.effect.healing". Language independent, and
//      it is what actually separates the three potions on 1.21.
//   2. ItemStack.getTranslationKey(), which still identifies the totem and
//      still carries the effect on pre-1.20.5 servers.
//   3. Text.getString(), the localized display name. Last resort, and only
//      reachable by name because Text.getString() comes from Brigadier's
//      Message interface -- third-party, so it is never obfuscated.
//
// All three are concatenated and matched as substrings, which then covers
// splash, lingering, drinkable, every tier and every version without walking a
// single component or NBT tag.
//
// Runs from GameLoopEvent throttled to the world-tick rate -- the pump that in
// MC 1.21 is the game-loop thread, so no bytecode is transformed anywhere.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/setting/SlotMapSetting.h"
#include "modules/shared/movement_idle.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <sdk/mappings/mappings.hpp>

#include <cctype>
#include <cstring>
#include <jni.h>
#include <random>
#include <set>
#include <string>

namespace vanta::modules {

namespace {

constexpr float TICK_MS = 50.f;

// Container slot space of the player screen: 9-35 is the main inventory and
// 36-44 the hotbar. A SWAP click carries the destination hotbar index in its
// button field, which is how one click moves an item without ever holding it.
constexpr int  MAIN_INV_FIRST = 9;
constexpr int  MAIN_INV_LAST  = 35;
constexpr jint PLAYER_SYNC_ID = 0;
constexpr jint SWAP_ACTION_ORDINAL = 2;

// Option order must match the labels handed to the SlotMapSetting below.
enum Kind : int { KIND_HEALTH = 0, KIND_SPEED = 1, KIND_STRENGTH = 2, KIND_TOTEM = 3 };

// Substrings of the combined identity string. Vanilla's own words come first;
// the English display words are there for the getString() fallback path.
// Strong/long tiers keep the same base word ("strong_healing" -> "healing"), so
// substring matching covers every tier for free.
bool key_matches(const std::string& key, int kind)
{
    auto has = [&key](const char* needle) {
        return key.find(needle) != std::string::npos;
    };
    switch (kind) {
    case KIND_HEALTH:   return has("healing");
    case KIND_SPEED:    return has("swiftness") || has("speed");   // vanilla calls it swiftness
    case KIND_STRENGTH: return has("strength");
    case KIND_TOTEM:    return has("totem") || has("undying");
    default:            return false;
    }
}

// Lowercased so the display-name fallback is not case sensitive.
void append_lower(std::string& out, const char* s)
{
    if (!s) return;
    if (!out.empty()) out.push_back('|');
    for (const char* p = s; *p; ++p)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
}

struct Cache {
    bool      tried = false;
    bool      ready = false;

    jclass    mc_class          = nullptr;
    jfieldID  mc_player         = nullptr;
    jfieldID  mc_screen         = nullptr;
    jfieldID  mc_interaction    = nullptr;
    jmethodID mc_set_screen     = nullptr;

    jfieldID  player_inventory  = nullptr;
    jmethodID inv_get_stack     = nullptr;

    jmethodID stack_is_empty    = nullptr;
    jmethodID stack_translation = nullptr;

    // Name path: the only one that still carries a potion's effect on 1.21.
    jmethodID stack_get_name    = nullptr;   // ItemStack.getName()  -> Text
    jmethodID text_get_content  = nullptr;   // Text.getContent()    -> TextContent
    jmethodID text_get_string   = nullptr;   // Text.getString()     -> String (Brigadier, unobfuscated)
    jclass    translatable_cls  = nullptr;   // TranslatableTextContent
    jmethodID translatable_key  = nullptr;   // .getKey()            -> String

    jmethodID click_slot        = nullptr;
    jobject   swap_action       = nullptr;   // global ref

    jclass    inv_screen_class  = nullptr;
    jmethodID inv_screen_ctor   = nullptr;
    jclass    handled_screen    = nullptr;
};

Cache g_c;

std::mt19937& rng() { static std::mt19937 g(std::random_device{}()); return g; }
int rand_int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }

jclass find_class(JNIEnv* env, const char* sig)
{
    jclass local = vanta::jvm::findClassAnyLoader(env, sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jclass g = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return g;
}

jfieldID field_either(JNIEnv* env, jclass cls, const char* yarn, const char* mojang, const char* sig)
{
    if (!cls) return nullptr;
    const bool mf = sdk::mappings::is_mojang_layout;
    for (const char* n : { mf ? mojang : yarn, mf ? yarn : mojang }) {
        jfieldID f = env->GetFieldID(cls, n, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (f) return f;
    }
    return nullptr;
}

jmethodID method_either(JNIEnv* env, jclass cls, const char* yarn, const char* mojang, const char* sig)
{
    if (!cls) return nullptr;
    const bool mf = sdk::mappings::is_mojang_layout;
    for (const char* n : { mf ? mojang : yarn, mf ? yarn : mojang }) {
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

    g_c.mc_class = find_class(env, sdk::mappings::minecraftclass_sig);
    if (!g_c.mc_class) return;

    g_c.mc_player      = env->GetFieldID(g_c.mc_class,
        sdk::mappings::player_name, sdk::mappings::player_sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_c.mc_screen      = env->GetFieldID(g_c.mc_class, "field_1755", "Lnet/minecraft/class_437;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_c.mc_interaction = env->GetFieldID(g_c.mc_class,
        sdk::mappings::interaction_manager_name, sdk::mappings::interaction_manager_sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_c.mc_set_screen  = method_either(env, g_c.mc_class, "method_1507", "setScreen",
                                       "(Lnet/minecraft/class_437;)V");

    if (jclass pe = find_class(env, sdk::mappings::player_entity_class_sig)) {
        g_c.player_inventory = field_either(env, pe, "field_7514", "inventory",
                                            "Lnet/minecraft/class_1661;");
        env->DeleteGlobalRef(pe);
    }
    if (jclass inv = find_class(env, "net/minecraft/class_1661")) {
        g_c.inv_get_stack = method_either(env, inv, "method_5438", "getItem",
                                          "(I)Lnet/minecraft/class_1799;");
        env->DeleteGlobalRef(inv);
    }
    if (jclass st = find_class(env, "net/minecraft/class_1799")) {
        g_c.stack_is_empty = method_either(env, st, "method_7960", "isEmpty", "()Z");
        g_c.stack_translation = method_either(env, st, "method_7922", "getDescriptionId",
                                              "()Ljava/lang/String;");
        g_c.stack_get_name = method_either(env, st, "method_7964", "getHoverName",
                                           "()Lnet/minecraft/class_2561;");
        env->DeleteGlobalRef(st);
    }

    if (jclass txt = find_class(env, "net/minecraft/class_2561")) {
        // TextContent was split out into its own type in 1.19; try that first
        // and fall back to the older shape where getContent() returned Text.
        g_c.text_get_content = method_either(env, txt, "method_10851", "getContents",
                                             "()Lnet/minecraft/class_7417;");
        if (!g_c.text_get_content)
            g_c.text_get_content = method_either(env, txt, "method_10851", "getContents",
                                                 "()Lnet/minecraft/class_2561;");
        // Brigadier's Message.getString(): third-party interface, never remapped.
        g_c.text_get_string = env->GetMethodID(txt, "getString", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteGlobalRef(txt);
    }

    g_c.translatable_cls = find_class(env, "net/minecraft/class_2588");
    if (g_c.translatable_cls) {
        g_c.translatable_key = method_either(env, g_c.translatable_cls, "method_11022", "getKey",
                                             "()Ljava/lang/String;");
    }

    if (jclass im = find_class(env, sdk::mappings::interaction_manager_class_sig)) {
        g_c.click_slot = method_either(env, im, "method_2906", "handleInventoryMouseClick",
            "(IIILnet/minecraft/class_1713;Lnet/minecraft/class_1657;)V");
        env->DeleteGlobalRef(im);
    }

    // SlotActionType.SWAP by ordinal: the enum's constant NAMES move between
    // mapping flavours but its declaration order does not.
    if (jclass sat = find_class(env, "net/minecraft/class_1713")) {
        jmethodID values = env->GetStaticMethodID(sat, "values", "()[Lnet/minecraft/class_1713;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (values) {
            jobjectArray arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(sat, values));
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (arr && env->GetArrayLength(arr) > SWAP_ACTION_ORDINAL) {
                jobject v = env->GetObjectArrayElement(arr, SWAP_ACTION_ORDINAL);
                if (v) { g_c.swap_action = env->NewGlobalRef(v); env->DeleteLocalRef(v); }
            }
            if (arr) env->DeleteLocalRef(arr);
        }
        env->DeleteGlobalRef(sat);
    }

    g_c.inv_screen_class = find_class(env, "net/minecraft/class_490");
    if (g_c.inv_screen_class) {
        g_c.inv_screen_ctor = env->GetMethodID(g_c.inv_screen_class, "<init>",
                                               "(Lnet/minecraft/class_1657;)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    g_c.handled_screen = find_class(env, "net/minecraft/class_465");

    // The name path is what separates the potions, but the module is still
    // usable without it (totems keep working off the translation key), so it is
    // not part of the readiness gate -- only of the diagnostic below.
    g_c.ready = g_c.mc_player && g_c.player_inventory && g_c.inv_get_stack
             && g_c.stack_is_empty && (g_c.stack_translation || g_c.stack_get_name)
             && g_c.click_slot && g_c.swap_action && g_c.mc_interaction;

    VLOG_INFO("refill", "cache: inv={} key={} name={} content={} tkey={} str={} click={} swap={} screen={} -> {}",
              g_c.inv_get_stack ? 1 : 0, g_c.stack_translation ? 1 : 0,
              g_c.stack_get_name ? 1 : 0, g_c.text_get_content ? 1 : 0,
              g_c.translatable_key ? 1 : 0, g_c.text_get_string ? 1 : 0,
              g_c.click_slot ? 1 : 0, g_c.swap_action ? 1 : 0,
              g_c.inv_screen_ctor ? 1 : 0,
              g_c.ready ? "ready" : "unusable");
}

// Appends a Java String's contents, lowercased, then releases it.
void append_jstring(JNIEnv* env, std::string& out, jstring s)
{
    if (!s) return;
    if (const char* c = env->GetStringUTFChars(s, nullptr)) {
        append_lower(out, c);
        env->ReleaseStringUTFChars(s, c);
    }
}

// Builds the combined identity string for a stack. Empty when the stack is
// empty or nothing could be resolved.
std::string stack_identity(JNIEnv* env, jobject stack)
{
    std::string out;

    if (g_c.stack_translation) {
        jstring k = static_cast<jstring>(env->CallObjectMethod(stack, g_c.stack_translation));
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (k) { append_jstring(env, out, k); env->DeleteLocalRef(k); }
    }

    if (!g_c.stack_get_name) return out;

    jobject name = env->CallObjectMethod(stack, g_c.stack_get_name);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
    if (!name) return out;

    // Preferred: the translation key behind the Text, which names the effect
    // without depending on the client's language.
    bool got_key = false;
    if (g_c.text_get_content && g_c.translatable_cls && g_c.translatable_key) {
        jobject content = env->CallObjectMethod(name, g_c.text_get_content);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (content) {
            if (env->IsInstanceOf(content, g_c.translatable_cls) == JNI_TRUE) {
                jstring tk = static_cast<jstring>(
                    env->CallObjectMethod(content, g_c.translatable_key));
                if (env->ExceptionCheck()) env->ExceptionClear();
                else if (tk) { append_jstring(env, out, tk); env->DeleteLocalRef(tk); got_key = true; }
            }
            env->DeleteLocalRef(content);
        }
    }

    // Fallback: the localized display name. Only reached when the content is
    // not translatable (a renamed item, or a mapping that did not resolve).
    if (!got_key && g_c.text_get_string) {
        jstring s = static_cast<jstring>(env->CallObjectMethod(name, g_c.text_get_string));
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (s) { append_jstring(env, out, s); env->DeleteLocalRef(s); }
    }

    env->DeleteLocalRef(name);
    return out;
}

constexpr int NO_KIND   = -1;
constexpr int SCAN_SIZE = 36;   // 0-8 hotbar, 9-35 main inventory

// Which of our kinds the stack at an inventory index is, or NO_KIND.
int slot_kind(JNIEnv* env, jobject inv, int index)
{
    jobject stack = env->CallObjectMethod(inv, g_c.inv_get_stack, index);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return NO_KIND; }
    if (!stack) return NO_KIND;

    int result = NO_KIND;
    const jboolean empty = env->CallBooleanMethod(stack, g_c.stack_is_empty);
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (empty != JNI_TRUE) {
        const std::string id = stack_identity(env, stack);
        for (int k = KIND_HEALTH; k <= KIND_TOTEM; ++k) {
            if (key_matches(id, k)) { result = k; break; }
        }

        // One line per distinct item ever seen. Costs nothing after warm-up and
        // means a mapping that resolves to the wrong string is visible in the
        // log instead of showing up as "it just does not refill".
        static std::set<std::string> seen;
        if (!id.empty() && seen.size() < 64 && seen.insert(id).second)
            VLOG_INFO("refill", "item identity: {}", id);
    }
    env->DeleteLocalRef(stack);
    return result;
}

// One pass over the inventory per pump. Resolving an item's identity costs
// several JNI calls, so re-reading the whole inventory before every swap is
// what made this crawl: a burst of nine swaps used to mean nine full rescans,
// over two thousand JNI calls. Scanning once into this and keeping it in step
// with the swaps costs thirty-six.
struct InvScan {
    int kind[SCAN_SIZE];

    InvScan() { for (int i = 0; i < SCAN_SIZE; ++i) kind[i] = NO_KIND; }

    // Ranges are built separately so the idle case -- hotbar already stocked,
    // which is almost every tick -- only pays for nine slots instead of
    // thirty-six.
    void build_range(JNIEnv* env, jobject inv, int first, int last) {
        for (int i = first; i <= last && i < SCAN_SIZE; ++i) kind[i] = slot_kind(env, inv, i);
    }
    // A SWAP exchanges the two slots, so mirroring it here keeps the cache
    // truthful without going back to the JVM.
    void apply_swap(int a, int b) {
        const int t = kind[a]; kind[a] = kind[b]; kind[b] = t;
    }
};

} // namespace

class Refill : public Module {
public:
    Refill() : Module("Refill",
                      "Restocks hotbar slots from your inventory",
                      Category::Combat),
               slots_(this, "Slots", { "Health", "Speed", "Strength", "Totem" }) {
        registerSetting(&speed_);
        registerSetting(&mode_);
        registerSetting(&autoClose_);
        registerSetting(&randomize_);
        registerSetting(&health_);
        registerSetting(&speedPot_);
        registerSetting(&strength_);
        registerSetting(&totem_);
        registerSetting(&slots_);

        // Auto Close only means anything when the player opened the screen --
        // Auto Open already closes after itself.
        autoClose_.setVisibleIf([this]{ return mode_.index() == 1; });

        // The strip may only offer what is ticked above. Handing the predicate
        // over keeps the constraint defined in one place: the widget asks, and
        // a slot holding a kind that is switched off reads as empty without
        // anything having to go and clear it.
        slots_.setAllowedProvider([this]{
            std::uint32_t m = 0;
            if (health_.value())   m |= 1u << KIND_HEALTH;
            if (speedPot_.value()) m |= 1u << KIND_SPEED;
            if (strength_.value()) m |= 1u << KIND_STRENGTH;
            if (totem_.value())    m |= 1u << KIND_TOTEM;
            return m;
        });
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){ pump(e.deltaMs); });
    }

    void onDisable() override { reset_session(); backoff_ = 0; }

private:
    // Higher is faster: the delay between actions is 6 - speed ticks, so 5 is
    // one tick between steps and 1 is a leisurely quarter second.
    IntSetting  speed_     {this, "Speed", 3, 1, 5};
    EnumSetting mode_      {this, "Mode", { "Auto Open", "On Open" }, 0};
    BoolSetting autoClose_ {this, "Auto Close", true};

    // Spread the delays instead of using the same gap every time. A fixed
    // cadence between container actions is the pattern the inventory checks
    // key on -- the same lesson AutoTotem learned.
    BoolSetting randomize_ {this, "Randomization", true};

    BoolSetting health_    {this, "Health",   true};
    BoolSetting speedPot_  {this, "Speed Pot", false};
    BoolSetting strength_  {this, "Strength", false};
    BoolSetting totem_     {this, "Totem",    true};

    SlotMapSetting slots_;

    enum class Stage { Idle, Priming, Cooldown, Closing };
    Stage stage_       = Stage::Idle;
    int   clock_       = 0;
    int   target_slot_ = -1;   // hotbar index being filled
    int   source_slot_ = -1;   // container slot the item came from
    bool  we_opened_   = false;
    bool  did_work_    = false;   // a swap landed during this screen session
    bool  filter_held_ = false;

    // Stand-still gate.
    //
    // The first version of this waited AFTER opening the screen, and it still
    // flagged with input=true even though the keys and the sprint flag both
    // read clear. The order was the bug: the client only tells the server about
    // its input from the movement packets, and by the time a screen is up the
    // last thing the server heard was the moving PlayerInput from the tick
    // before. A real player releases the keys FIRST and opens the inventory
    // second, so the non-moving PlayerInput is already on the wire.
    //
    // So the stop is now forced before anything is opened, and the wait happens
    // with no screen up at all -- which is the one arrangement where vanilla is
    // certainly still ticking input and sending the change.
    int  idle_ticks_ = 0;
    int  hold_ticks_ = 0;
    int  backoff_    = 0;
    int  session_ticks_ = 0;
    static constexpr int REQUIRED_IDLE_TICKS = 2;
    static constexpr int MAX_HOLD_TICKS      = 20;   // 1 s before giving up
    static constexpr int BACKOFF_TICKS       = 40;   // 2 s before retrying
    static constexpr int MAX_SESSION_TICKS   = 60;   // 3 s hard ceiling

    // The filter is what keeps Windows key repeat from re-pressing what
    // suppress_movement() just cleared -- without it the clear is undone within
    // about 30 ms. Owner-keyed so releasing it never drops AutoTotem's lock.
    void hold_filter()
    {
        if (filter_held_) return;
        ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_REFILL, 2);
        filter_held_ = true;
    }

    void release_filter()
    {
        if (!filter_held_) return;
        ::vanta::vgui::bridge::setInputFilterFor(::vanta::vgui::bridge::FILTER_OWNER_REFILL, 0);
        filter_held_ = false;
    }

    // The close has its own stand-still gate, so it needs its own hold budget:
    // arriving here after the click gate already exhausted one would make the
    // close fire instantly into MultiActionsD.
    void begin_closing(int delay)
    {
        clock_      = delay;
        hold_ticks_ = 0;
        stage_      = Stage::Closing;
    }

    // Always releases the filter. Every exit from a cycle goes through here, so
    // there is no path on which the player's keyboard stays swallowed.
    void reset_session()
    {
        release_filter();
        we_opened_     = false;
        did_work_      = false;
        idle_ticks_    = 0;
        hold_ticks_    = 0;
        session_ticks_ = 0;
        stage_         = Stage::Idle;
    }

    void abort_cycle()
    {
        backoff_ = BACKOFF_TICKS;
        if (we_opened_) begin_closing(1);
        else            reset_session();
    }

    // How many swaps may go out inside a single tick. The server processes
    // every packet it receives within the tick it receives them, so bursting is
    // the lever that actually makes this fast -- pumping more often than the
    // tick rate would not, since the extra packets just queue for the same tick.
    // At 5 the whole hotbar goes in one tick, which is the ceiling.
    int burst() const {
        switch (speed_.value()) {
        case 5:  return SlotMapSetting::SLOTS;
        case 4:  return 3;
        case 3:  return 2;
        default: return 1;
        }
    }

    // Ticks to idle between bursts. Zero at speed 5, so there is no gap at all.
    int step_delay() const {
        static const int base[6] = { 0, 4, 3, 2, 1, 0 };   // indexed by speed
        const int s = speed_.value() < 1 ? 1 : (speed_.value() > 5 ? 5 : speed_.value());
        int d = base[s];
        // Randomization still applies at speed 5: the burst has already emptied
        // the work list by then, so a 0-1 tick jitter costs nothing measurable
        // and keeps the irregular spacing the setting exists for.
        if (randomize_.value()) d += rand_int(0, 1);
        return d;
    }

    void pump(float delta_ms)
    {
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

        if (!isEnabled() || ::vanta::vgui::bridge::isOverlayVisible()) {
            reset_session();
            return;
        }

        jfieldID inst = env->GetStaticFieldID(g_c.mc_class,
            sdk::mappings::minecraftclient_name, sdk::mappings::minecraftclient_sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!inst) return;

        jobject mc = env->GetStaticObjectField(g_c.mc_class, inst);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!mc) return;

        jobject player = env->GetObjectField(mc, g_c.mc_player);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }
        if (player) {
            step(env, mc, player);
            env->DeleteLocalRef(player);
        } else {
            reset_session();
        }
        env->DeleteLocalRef(mc);
    }

    bool inventory_open(JNIEnv* env, jobject mc) const
    {
        if (!g_c.mc_screen || !g_c.handled_screen) return false;
        jobject s = env->GetObjectField(mc, g_c.mc_screen);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!s) return false;
        const bool handled = env->IsInstanceOf(s, g_c.handled_screen) == JNI_TRUE;
        env->DeleteLocalRef(s);
        return handled;
    }

    void step(JNIEnv* env, jobject mc, jobject player)
    {
        jobject inv = env->GetObjectField(player, g_c.player_inventory);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!inv) return;

        // Hard ceiling on how long one cycle may hold the player's keyboard.
        // A bug upstream must never be able to take their input hostage.
        if (filter_held_ && ++session_ticks_ > MAX_SESSION_TICKS) {
            VLOG_WARN("refill", "session exceeded {} ticks -- releasing input", MAX_SESSION_TICKS);
            backoff_ = BACKOFF_TICKS;
            if (we_opened_ && g_c.mc_set_screen) {
                env->CallVoidMethod(mc, g_c.mc_set_screen, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            reset_session();
            env->DeleteLocalRef(inv);
            return;
        }

        switch (stage_) {
        case Stage::Idle:     step_idle(env, mc, player, inv);           break;
        case Stage::Priming:  step_priming(env, mc, player);             break;
        case Stage::Cooldown: if (--clock_ <= 0) stage_ = Stage::Idle;   break;
        case Stage::Closing:  step_closing(env, mc);                     break;
        }
        env->DeleteLocalRef(inv);
    }

    // Picks the first assigned hotbar slot that is not already holding its kind
    // and a source for it in the main inventory. Reads only the cached scan, so
    // it is free to call once per swap inside a burst.
    bool find_job(const InvScan& scan)
    {
        for (int hb = 0; hb < SlotMapSetting::SLOTS; ++hb) {
            const int kind = slots_.slot(hb);
            if (kind == SlotMapSetting::EMPTY) continue;
            if (scan.kind[hb] == kind) continue;              // already stocked

            for (int src = MAIN_INV_FIRST; src <= MAIN_INV_LAST; ++src) {
                if (scan.kind[src] != kind) continue;
                target_slot_ = hb;
                source_slot_ = src;
                return true;
            }
        }
        return false;
    }

    // True when at least one square in the strip is assigned to a kind that is
    // still switched on -- reading through slots_, so a kind ticked off already
    // reads as empty here.
    bool any_assigned() const
    {
        for (int hb = 0; hb < SlotMapSetting::SLOTS; ++hb)
            if (slots_.slot(hb) != SlotMapSetting::EMPTY) return true;
        return false;
    }

    bool hotbar_wants(const InvScan& scan) const
    {
        for (int hb = 0; hb < SlotMapSetting::SLOTS; ++hb) {
            const int kind = slots_.slot(hb);
            if (kind == SlotMapSetting::EMPTY) continue;
            if (scan.kind[hb] != kind) return true;
        }
        return false;
    }

    // Bring the player to a genuine stop, with NO screen open, and only then
    // open one. Waiting here rather than after the open is the whole fix: with
    // no screen up, vanilla is certainly still ticking input, so clearing the
    // bindings actually produces a non-moving PlayerInput on the wire. Waiting
    // after the open left the server holding the last moving PlayerInput from
    // the tick before, which is exactly what Grim read.
    //
    // The player really does stop for these two ticks. That is not a side
    // effect to be minimised -- it is the behaviour being reproduced.
    void step_priming(JNIEnv* env, jobject mc, jobject player)
    {
        hold_filter();
        ::vanta::shared::suppress_movement(env, mc);

        if (::vanta::shared::movement_is_idle(env, mc)) ++idle_ticks_;
        else                                            idle_ticks_ = 0;

        if (idle_ticks_ < REQUIRED_IDLE_TICKS) {
            if (++hold_ticks_ < MAX_HOLD_TICKS) return;
            // Abandon rather than open into a certain flag. This is the opposite
            // of what AutoTotem does on the same timeout, and deliberately:
            // being late with a totem gets you killed, being late with a potion
            // costs nothing.
            VLOG_WARN("refill", "priming never settled -- skipping this cycle");
            abort_cycle();
            return;
        }

        if (!open_screen(env, mc, player)) { abort_cycle(); return; }
        we_opened_ = true;
        stage_     = Stage::Idle;   // next pump bursts, keys still pinned
    }

    void step_idle(JNIEnv* env, jobject mc, jobject player, jobject inv)
    {
        if (backoff_ > 0) { --backoff_; return; }

        // Widen the scan only as far as the answer needs it: nothing assigned
        // costs zero reads, a stocked hotbar costs nine, and only a slot that
        // actually needs filling pays for the main inventory.
        InvScan scan;
        bool work = false;
        if (any_assigned()) {
            scan.build_range(env, inv, 0, SlotMapSetting::SLOTS - 1);
            if (hotbar_wants(scan)) {
                scan.build_range(env, inv, MAIN_INV_FIRST, MAIN_INV_LAST);
                work = find_job(scan);
            }
        }

        if (!inventory_open(env, mc)) {
            if (mode_.index() == 1) { reset_session(); return; }   // On Open: the player's move
            // Auto Open: do not flash a screen open with nothing to put in it.
            if (!work) { reset_session(); return; }
            // Stop moving BEFORE opening anything. See Priming.
            idle_ticks_ = 0;
            hold_ticks_ = 0;
            stage_      = Stage::Priming;
            return;
        }

        // Screen is up. Keep the keys pinned for as long as it is ours, so the
        // player cannot walk out from under the container action we are about
        // to take.
        if (we_opened_) { hold_filter(); ::vanta::shared::suppress_movement(env, mc); }

        if (::vanta::shared::movement_is_idle(env, mc)) ++idle_ticks_;
        else                                            idle_ticks_ = 0;

        if (!work) {
            // Nothing left to fill. Close if we are the ones holding the screen
            // open, or if the player opened it and asked us to close after we
            // actually did something -- gated on did_work_ so opening your own
            // inventory with a full hotbar does not slam it shut in your face.
            if (we_opened_ || (mode_.index() == 1 && autoClose_.value() && did_work_))
                begin_closing(step_delay());
            return;
        }

        // Second barrier. Priming already put a non-moving PlayerInput on the
        // wire, so this is just insurance against the player having got a key
        // through in between.
        if (idle_ticks_ < 1) {
            if (++hold_ticks_ < MAX_HOLD_TICKS) return;
            VLOG_WARN("refill", "stillness gate never settled -- skipping this cycle");
            abort_cycle();
            return;
        }

        // Swap until the budget or the work runs out. The cache is kept in step
        // with each swap, so every iteration sees the inventory as it now is.
        for (int budget = burst(); work && budget > 0; --budget) {
            if (!click_swap(env, mc, player)) break;
            scan.apply_swap(target_slot_, source_slot_);
            did_work_ = true;
            work = find_job(scan);
        }

        if (work) {
            // More to do than the budget allowed: come back after the gap.
            const int d = step_delay();
            if (d > 0) { clock_ = d; stage_ = Stage::Cooldown; }
            return;
        }

        if (we_opened_ || (mode_.index() == 1 && autoClose_.value() && did_work_))
            begin_closing(step_delay());
    }

    void step_closing(JNIEnv* env, jobject mc)
    {
        if (--clock_ > 0) return;

        // MultiActionsD is the close-side twin of MultiActionsC -- "Closed
        // inventory while moving", same three booleans, same knownInput. Grim
        // deliberately does NOT cancel that packet, so an ungated close is a
        // pure flag with nothing to show for it.
        if (!::vanta::shared::movement_is_idle(env, mc) && ++hold_ticks_ < MAX_HOLD_TICKS) {
            clock_ = 1;
            return;
        }

        if (g_c.mc_set_screen) {
            env->CallVoidMethod(mc, g_c.mc_set_screen, nullptr);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        reset_session();
    }

    bool open_screen(JNIEnv* env, jobject mc, jobject player)
    {
        if (!g_c.inv_screen_class || !g_c.inv_screen_ctor || !g_c.mc_set_screen) return false;
        jobject scr = env->NewObject(g_c.inv_screen_class, g_c.inv_screen_ctor, player);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!scr) return false;
        env->CallVoidMethod(mc, g_c.mc_set_screen, scr);
        const bool ok = !env->ExceptionCheck();
        if (!ok) env->ExceptionClear();
        env->DeleteLocalRef(scr);
        return ok;
    }

    // One SWAP click: source container slot, destination hotbar index in the
    // button field. Exactly the shape vanilla emits when you press a number key
    // with the inventory open.
    bool click_swap(JNIEnv* env, jobject mc, jobject player)
    {
        jobject im = env->GetObjectField(mc, g_c.mc_interaction);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!im) return false;

        env->CallVoidMethod(im, g_c.click_slot,
                            PLAYER_SYNC_ID,
                            static_cast<jint>(source_slot_),
                            static_cast<jint>(target_slot_),
                            g_c.swap_action,
                            player);
        const bool ok = !env->ExceptionCheck();
        if (!ok) env->ExceptionClear();
        env->DeleteLocalRef(im);
        return ok;
    }
};

VANTA_REGISTER_MODULE(Refill);

} // namespace vanta::modules
