// autototem.cpp — JNI reflection cache + lifecycle for AutoTotem.
//
// The state machine lives in autototem_tick.cpp.

#include "autototem.h"
#include "autototem_tick.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <string>
#include <mutex>

namespace dt::modules::autototem {

static jni_cache  g_cache;
static std::mutex s_cache_mx;

const jni_cache& get_cache()     { return g_cache; }
jni_cache&       mutable_cache() { return g_cache; }

// ── small reflection helpers ─────────────────────────────────────────────────

static bool cache_class(JNIEnv* env, const char* sig, jclass& out)
{
    if (!sig) return false;
    jclass local = dt::jvm::findClassAnyLoader(env, sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    out = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return out != nullptr;
}

static jmethodID cache_method(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

static jfieldID cache_field(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID f = env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return f;
}

static jobject cache_static_object(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID fid = env->GetStaticFieldID(cls, name, sig);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!fid) return nullptr;

    jobject local = env->GetStaticObjectField(cls, fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!local) return nullptr;

    jobject global = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return global;
}

// Enum fallback: values()[ordinal]. Intermediary renames enum CONSTANTS too,
// and unlike class/method names those do shift between 1.21.x drops — but
// declaration order is part of the protocol and does not.
static jobject cache_enum_by_ordinal(JNIEnv* env, jclass enum_cls,
    const char* class_sig, int ordinal)
{
    if (!enum_cls || !class_sig || ordinal < 0) return nullptr;

    const std::string values_sig = "()[L" + std::string(class_sig) + ";";
    jmethodID values = env->GetStaticMethodID(enum_cls, "values", values_sig.c_str());
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!values) return nullptr;

    auto arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(enum_cls, values));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!arr) return nullptr;

    jobject global = nullptr;
    if (env->GetArrayLength(arr) > ordinal) {
        jobject local = env->GetObjectArrayElement(arr, ordinal);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (local) { global = env->NewGlobalRef(local); env->DeleteLocalRef(local); }
    }
    env->DeleteLocalRef(arr);
    return global;
}

// Finds the single boolean INSTANCE field on a class.
//
// Used for KeyBinding.pressed: the intermediary name for it is a guess
// (mappings.hpp says so outright), but the shape is not — KeyBinding declares
// exactly one boolean. Returns nullptr unless there is exactly one candidate,
// so an ambiguous class never silently picks wrong.
static jfieldID find_sole_boolean_field(JNIEnv* env, jclass target)
{
    if (!target) return nullptr;

    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!class_cls || !field_cls) return nullptr;

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_type   = env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;");
    jmethodID get_mods   = env->GetMethodID(field_cls, "getModifiers", "()I");
    jmethodID get_name   = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!get_fields || !get_type || !get_mods || !get_name) return nullptr;

    auto arr = static_cast<jobjectArray>(env->CallObjectMethod(target, get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!arr) return nullptr;

    constexpr jint ACC_STATIC = 0x0008;
    jfieldID found = nullptr;
    int      hits  = 0;

    const jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i < n; ++i) {
        jobject f = env->GetObjectArrayElement(arr, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!f) continue;

        const jint mods = env->CallIntMethod(f, get_mods);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }
        if (mods & ACC_STATIC) { env->DeleteLocalRef(f); continue; }

        jobject ftype = env->CallObjectMethod(f, get_type);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }
        if (!ftype) { env->DeleteLocalRef(f); continue; }

        auto tname = static_cast<jstring>(env->CallObjectMethod(ftype, get_name));
        if (env->ExceptionCheck()) env->ExceptionClear();
        bool is_bool = false;
        if (tname) {
            if (const char* utf = env->GetStringUTFChars(tname, nullptr)) {
                is_bool = (std::string(utf) == "boolean");
                env->ReleaseStringUTFChars(tname, utf);
            }
            env->DeleteLocalRef(tname);
        }
        env->DeleteLocalRef(ftype);

        if (is_bool) {
            if (jfieldID fid = env->FromReflectedField(f)) { found = fid; ++hits; }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(f);
    }
    env->DeleteLocalRef(arr);

    return (hits == 1) ? found : nullptr;
}

// ── cache build ──────────────────────────────────────────────────────────────

void init_cache(JNIEnv* env)
{
    if (!env || g_cache.initialized) return;
    std::scoped_lock lk(s_cache_mx);
    if (g_cache.initialized) return;

    using namespace sdk::mappings;

    cache_class(env, minecraftclass_sig, g_cache.minecraft_class);
    if (g_cache.minecraft_class) {
        g_cache.mc_player_fid              = cache_field(env, g_cache.minecraft_class, player_name, player_sig);
        g_cache.mc_current_screen_fid      = cache_field(env, g_cache.minecraft_class, minecraft_current_screen_name, minecraft_current_screen_sig);
        g_cache.mc_interaction_manager_fid = cache_field(env, g_cache.minecraft_class, interaction_manager_name, interaction_manager_sig);
        g_cache.mc_options_fid             = cache_field(env, g_cache.minecraft_class, game_options_name, game_options_sig);
        g_cache.mc_crosshair_target_fid    = cache_field(env, g_cache.minecraft_class, crosshair_target_name, crosshair_target_sig);
        g_cache.mc_set_screen_mid          = cache_method(env, g_cache.minecraft_class, minecraft_set_screen_name, minecraft_set_screen_sig);
    }

    cache_class(env, clientplayerentity_class_sig, g_cache.client_player_class);
    if (g_cache.client_player_class) {
        g_cache.player_network_handler_fid =
            cache_field(env, g_cache.client_player_class, player_network_handler_name, player_network_handler_sig);
    }

    cache_class(env, player_entity_class_sig, g_cache.player_entity_class);
    if (g_cache.player_entity_class) {
        g_cache.player_inventory_fid =
            cache_field(env, g_cache.player_entity_class, player_inventory_name, player_inventory_sig);
    }

    cache_class(env, living_entity_class_sig, g_cache.living_entity_class);
    if (g_cache.living_entity_class) {
        g_cache.get_main_hand_stack_mid = cache_method(env, g_cache.living_entity_class,
            living_entity_get_main_hand_name, living_entity_get_main_hand_sig);
        g_cache.get_off_hand_stack_mid  = cache_method(env, g_cache.living_entity_class,
            living_entity_get_off_hand_name, living_entity_get_off_hand_sig);
    }

    cache_class(env, player_inventory_class_sig, g_cache.inventory_class);
    if (g_cache.inventory_class) {
        g_cache.inventory_selected_slot_fid = cache_field(env, g_cache.inventory_class,
            inventory_selected_slot_name, inventory_selected_slot_sig);
        g_cache.inventory_get_stack_mid = cache_method(env, g_cache.inventory_class,
            inventory_get_stack_name, inventory_get_stack_sig);
    }

    cache_class(env, itemstack_class_sig, g_cache.itemstack_class);
    if (g_cache.itemstack_class) {
        g_cache.itemstack_is_empty_mid = cache_method(env, g_cache.itemstack_class, itemstack_is_empty_name, itemstack_is_empty_sig);
        g_cache.itemstack_get_item_mid = cache_method(env, g_cache.itemstack_class, itemstack_get_item_name, itemstack_get_item_sig);
    }

    cache_class(env, item_class_sig, g_cache.item_class);
    if (g_cache.item_class) {
        g_cache.item_get_translation_key_mid = cache_method(env, g_cache.item_class,
            item_get_translation_key_name, item_get_translation_key_sig);
    }

    cache_class(env, interaction_manager_class_sig, g_cache.interaction_manager_class);
    if (g_cache.interaction_manager_class) {
        g_cache.sync_selected_slot_mid = cache_method(env, g_cache.interaction_manager_class,
            sync_selected_slot_name, sync_selected_slot_sig);
        g_cache.click_slot_mid = cache_method(env, g_cache.interaction_manager_class,
            click_slot_name, click_slot_sig);
    }

    // SlotActionType.SWAP — named static, else ordinal 2.
    // Order: PICKUP, QUICK_MOVE, SWAP, CLONE, THROW, QUICK_CRAFT, PICKUP_ALL.
    {
        jclass sat_cls = nullptr;
        if (cache_class(env, slot_action_type_class_sig, sat_cls)) {
            g_cache.slot_action_swap_ref = cache_static_object(env, sat_cls,
                slot_action_swap_name, slot_action_swap_sig);
            if (!g_cache.slot_action_swap_ref)
                g_cache.slot_action_swap_ref = cache_enum_by_ordinal(env, sat_cls,
                    slot_action_type_class_sig, 2);
            env->DeleteGlobalRef(sat_cls);
        }
    }

    cache_class(env, inventory_screen_class_sig, g_cache.inventory_screen_class);
    if (g_cache.inventory_screen_class) {
        g_cache.inventory_screen_ctor = cache_method(env, g_cache.inventory_screen_class,
            "<init>", inventory_screen_init_sig);
    }

    // PlayerActionC2SPacket + its three constructor arguments.
    cache_class(env, player_action_c2s_packet_class_sig, g_cache.player_action_packet_class);
    if (g_cache.player_action_packet_class) {
        const std::string ctor_sig =
            "(L" + std::string(player_action_c2s_packet_action_class_sig) + ";"
            "L" + std::string(block_pos_class_sig) + ";"
            "L" + std::string(direction_class_sig) + ";)V";
        g_cache.player_action_packet_ctor = cache_method(env,
            g_cache.player_action_packet_class, "<init>", ctor_sig.c_str());
    }

    // Action.SWAP_ITEM_WITH_OFFHAND — named, else ordinal 6.
    //   0 START_DESTROY_BLOCK  1 ABORT_DESTROY_BLOCK  2 STOP_DESTROY_BLOCK
    //   3 DROP_ALL_ITEMS       4 DROP_ITEM            5 RELEASE_USE_ITEM
    //   6 SWAP_ITEM_WITH_OFFHAND
    {
        jclass action_cls = nullptr;
        if (cache_class(env, player_action_c2s_packet_action_class_sig, action_cls)) {
            g_cache.swap_offhand_action_ref = cache_static_object(env, action_cls,
                swap_item_with_offhand_action_name, swap_item_with_offhand_action_sig);
            if (!g_cache.swap_offhand_action_ref)
                g_cache.swap_offhand_action_ref = cache_enum_by_ordinal(env, action_cls,
                    player_action_c2s_packet_action_class_sig, 6);
            env->DeleteGlobalRef(action_cls);
        }
    }

    // BlockPos.ORIGIN — named static, else construct BlockPos(0,0,0).
    {
        jclass bp_cls = nullptr;
        if (cache_class(env, block_pos_class_sig, bp_cls)) {
            g_cache.block_pos_origin_ref =
                cache_static_object(env, bp_cls, block_pos_origin_name, block_pos_origin_sig);
            if (!g_cache.block_pos_origin_ref) {
                if (jmethodID ctor = cache_method(env, bp_cls, "<init>", "(III)V")) {
                    jobject local = env->NewObject(bp_cls, ctor, 0, 0, 0);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    else if (local) {
                        g_cache.block_pos_origin_ref = env->NewGlobalRef(local);
                        env->DeleteLocalRef(local);
                    }
                }
            }
            env->DeleteGlobalRef(bp_cls);
        }
    }

    // Direction.DOWN — named static, else ordinal 0.
    {
        jclass dir_cls = nullptr;
        if (cache_class(env, direction_class_sig, dir_cls)) {
            g_cache.direction_down_ref =
                cache_static_object(env, dir_cls, direction_down_name, direction_down_sig);
            if (!g_cache.direction_down_ref)
                g_cache.direction_down_ref = cache_enum_by_ordinal(env, dir_cls,
                    direction_class_sig, 0);
            env->DeleteGlobalRef(dir_cls);
        }
    }

    // CloseHandledScreenC2SPacket(int) — Silent mode's session close.
    cache_class(env, "net/minecraft/class_2815", g_cache.close_screen_packet_class);
    if (g_cache.close_screen_packet_class) {
        g_cache.close_screen_packet_ctor =
            cache_method(env, g_cache.close_screen_packet_class, "<init>", "(I)V");
    }

    cache_class(env, network_handler_class_sig, g_cache.network_handler_class);
    if (g_cache.network_handler_class) {
        g_cache.send_packet_mid = cache_method(env, g_cache.network_handler_class,
            send_packet_name, send_packet_sig);
        if (!g_cache.send_packet_mid) {
            // The yarn name genuinely moves between 1.21.x drops — in 1.20.2+
            // it was hoisted into ClientCommonNetworkHandler and renumbered
            // more than once. mappings.hpp carries method_2883, which is not
            // the live one on every host.
            static const char* kSendCandidates[] = {
                "method_45729", "method_52787", "method_2883", "method_36117", "send", nullptr
            };
            for (int i = 0; kSendCandidates[i]; ++i) {
                g_cache.send_packet_mid = cache_method(env, g_cache.network_handler_class,
                                                       kSendCandidates[i], send_packet_sig);
                if (g_cache.send_packet_mid) {
                    VLOG_INFO("autototem", "sendPacket resolved as {}", kSendCandidates[i]);
                    break;
                }
            }
        }
    }

    // Digging gate.
    cache_class(env, game_options_class_sig, g_cache.game_options_class);
    if (g_cache.game_options_class) {
        g_cache.options_attack_key_fid = cache_field(env, g_cache.game_options_class,
            game_options_attack_key_name, game_options_attack_key_sig);
    }
    cache_class(env, block_hit_result_class_sig, g_cache.block_hit_result_class);

    // KeyBinding.
    cache_class(env, key_binding_class_sig, g_cache.key_binding_class);
    if (g_cache.key_binding_class) {
        g_cache.key_binding_get_translation_mid = cache_method(env, g_cache.key_binding_class,
            key_binding_get_translation_key_name, key_binding_get_translation_key_sig);
        g_cache.key_binding_is_pressed_mid = cache_method(env, g_cache.key_binding_class,
            key_binding_is_pressed_name, key_binding_is_pressed_sig);
        g_cache.key_binding_set_pressed_mid = cache_method(env, g_cache.key_binding_class,
            key_binding_set_pressed_name, key_binding_set_pressed_sig);

        g_cache.key_binding_times_pressed_fid =
            cache_field(env, g_cache.key_binding_class, "field_1661", "I");
        if (!g_cache.key_binding_times_pressed_fid)
            g_cache.key_binding_times_pressed_fid =
                cache_field(env, g_cache.key_binding_class, "timesPressed", "I");

        g_cache.key_binding_pressed_fid =
            cache_field(env, g_cache.key_binding_class, key_binding_pressed_field_name, "Z");
        if (!g_cache.key_binding_pressed_fid)
            g_cache.key_binding_pressed_fid = find_sole_boolean_field(env, g_cache.key_binding_class);
    }

    // Report by NAME. Logging raw pointers once hid a null sendPacket in plain
    // sight while the module silently did nothing for an entire session.
    struct { const char* name; const void* p; } required[] = {
        { "PlayerInventory.getStack",     (const void*)g_cache.inventory_get_stack_mid },
        { "PlayerInventory.selectedSlot", (const void*)g_cache.inventory_selected_slot_fid },
        { "Item.getTranslationKey",       (const void*)g_cache.item_get_translation_key_mid },
        { "sendPacket",                   (const void*)g_cache.send_packet_mid },
        { "networkHandler field",         (const void*)g_cache.player_network_handler_fid },
        { "PlayerActionC2SPacket.<init>", (const void*)g_cache.player_action_packet_ctor },
        { "Action.SWAP_ITEM_WITH_OFFHAND",(const void*)g_cache.swap_offhand_action_ref },
        { "clickSlot",                    (const void*)g_cache.click_slot_mid },
        { "SlotActionType.SWAP",          (const void*)g_cache.slot_action_swap_ref },
    };

    bool complete = true;
    for (const auto& r : required) {
        if (!r.p) {
            VLOG_ERR("autototem", "MISSING mapping: {}", r.name);
            complete = false;
        }
    }
    g_cache.initialized = complete;
    VLOG_INFO("autototem", complete ? "cache ready — all required mappings resolved"
                                    : "cache INCOMPLETE — will retry on next check");
    VLOG_INFO("autototem", "KeyBinding.pressed field: {}",
              g_cache.key_binding_pressed_fid ? "resolved" : "NOT FOUND");
}

void shutdown_cache()
{
    autototem_tick::reset();

    std::scoped_lock lk(s_cache_mx);
    if (!g_cache.initialized) return;
    if (!dt::jvm::ctx().vm.load()) { g_cache = jni_cache{}; return; }

    dt::jvm::JniScope scope;
    if (!scope) { g_cache = jni_cache{}; return; }
    JNIEnv* env = scope.get();

    auto drop_class = [&](jclass& c) { if (c) { env->DeleteGlobalRef(c); c = nullptr; } };
    auto drop_obj   = [&](jobject& o) { if (o) { env->DeleteGlobalRef(o); o = nullptr; } };

    drop_class(g_cache.minecraft_class);
    drop_class(g_cache.client_player_class);
    drop_class(g_cache.player_entity_class);
    drop_class(g_cache.living_entity_class);
    drop_class(g_cache.inventory_class);
    drop_class(g_cache.itemstack_class);
    drop_class(g_cache.item_class);
    drop_class(g_cache.interaction_manager_class);
    drop_class(g_cache.inventory_screen_class);
    drop_class(g_cache.network_handler_class);
    drop_class(g_cache.player_action_packet_class);
    drop_class(g_cache.close_screen_packet_class);
    drop_class(g_cache.game_options_class);
    drop_class(g_cache.block_hit_result_class);
    drop_class(g_cache.key_binding_class);

    drop_obj(g_cache.slot_action_swap_ref);
    drop_obj(g_cache.swap_offhand_action_ref);
    drop_obj(g_cache.block_pos_origin_ref);
    drop_obj(g_cache.direction_down_ref);

    g_cache = jni_cache{};
}

void ensure_installed()
{
    if (g_shutting_down.load(std::memory_order_acquire)) return;
    if (!dt::jvm::ctx().vm.load()) return;

    JNIEnv* env = dt::jvm::thisThreadEnv();
    if (!env) return;

    if (!settings::enabled.load(std::memory_order_relaxed)) return;
    if (!g_cache.initialized) init_cache(env);
}

} // namespace dt::modules::autototem
