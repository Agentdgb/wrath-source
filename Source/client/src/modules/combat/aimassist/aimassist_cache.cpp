

#include "aimassist.h"
#include "aimassist_debug.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace vanta::modules::aimassist {

namespace {

JniCache          g_cache;
static std::mutex s_cache_mx;

jclass cache_class(JNIEnv* env, const char* binary_name)
{
    if (!binary_name) return nullptr;
    jclass local = vanta::jvm::findClassAnyLoader(env, binary_name);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

jmethodID cache_method(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

jmethodID cache_method_either(JNIEnv* env, jclass cls,
                              const char* yarn, const char* mojang, const char* sig)
{
    jmethodID m = cache_method(env, cls, yarn, sig);
    if (!m) m = cache_method(env, cls, mojang, sig);
    return m;
}

jfieldID cache_field(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
    if (!cls || !name || !sig) return nullptr;
    jfieldID f = env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return f;
}

jfieldID cache_field_either(JNIEnv* env, jclass cls,
                            const char* yarn, const char* mojang, const char* sig)
{
    jfieldID f = cache_field(env, cls, yarn, sig);
    if (!f) f = cache_field(env, cls, mojang, sig);
    return f;
}

jfieldID find_sole_boolean_field(JNIEnv* env, jclass cls)
{
    jclass class_cls  = env->FindClass("java/lang/Class");
    jclass field_cls  = env->FindClass("java/lang/reflect/Field");
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
    int hits = 0;
    const jsize n = env->GetArrayLength(arr);
    for (jsize k = 0; k < n; ++k) {
        jobject fld = env->GetObjectArrayElement(arr, k);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!fld) continue;

        const jint mods = env->CallIntMethod(fld, get_mods);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
        if (mods & 0x0008) { env->DeleteLocalRef(fld); continue; }

        jobject ty = env->CallObjectMethod(fld, get_type);
        env->DeleteLocalRef(fld);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!ty) continue;

        jstring tname = static_cast<jstring>(env->CallObjectMethod(ty, type_name));
        env->DeleteLocalRef(ty);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        bool is_bool = false;
        if (tname) {
            const char* tn = env->GetStringUTFChars(tname, nullptr);
            if (tn) { is_bool = (std::strcmp(tn, "boolean") == 0);
                      env->ReleaseStringUTFChars(tname, tn); }
            env->DeleteLocalRef(tname);
        }
        if (!is_bool) continue;

        jstring jname = nullptr;
        {
            jobject fld2 = env->GetObjectArrayElement(arr, k);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fld2 = nullptr; }
            if (fld2) {
                jname = static_cast<jstring>(env->CallObjectMethod(fld2, get_name));
                env->DeleteLocalRef(fld2);
            }
        }
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
        if (hits > 1) break;
    }
    env->DeleteLocalRef(arr);
    return (hits == 1) ? found : nullptr;
}

void resolve_movement_keys(JNIEnv* env, jobject mc)
{
    if (g_cache.move_keys_resolved) return;
    if (!g_cache.mc_options_fid || !g_cache.key_binding_class
        || !g_cache.key_translation_mid) return;

    jobject options = env->GetObjectField(mc, g_cache.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;
    jclass opts_cls = env->GetObjectClass(options);

    jclass class_cls = env->FindClass("java/lang/Class");
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!class_cls || !field_cls) { env->DeleteLocalRef(options); return; }

    jmethodID get_fields = env->GetMethodID(class_cls, "getDeclaredFields",
                                            "()[Ljava/lang/reflect/Field;");
    jmethodID get_name   = env->GetMethodID(field_cls, "getName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }

    jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(opts_cls, get_fields));
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(options); return; }
    if (!arr) { env->DeleteLocalRef(options); return; }

    static const char* kWanted[4] = { "key.forward", "key.back", "key.left", "key.right" };
    const jsize n = env->GetArrayLength(arr);
    for (jsize k = 0; k < n; ++k) {
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
        jstring tk = static_cast<jstring>(env->CallObjectMethod(kb, g_cache.key_translation_mid));
        env->DeleteLocalRef(kb);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!tk) continue;
        const char* t = env->GetStringUTFChars(tk, nullptr);
        if (t) {
            for (int w = 0; w < 4; ++w)
                if (std::strcmp(t, kWanted[w]) == 0 && !g_cache.move_key_fids[w])
                    g_cache.move_key_fids[w] = fid;
            env->ReleaseStringUTFChars(tk, t);
        }
        env->DeleteLocalRef(tk);
    }
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(options);

    g_cache.move_keys_resolved = g_cache.move_key_fids[0] && g_cache.move_key_fids[1]
                              && g_cache.move_key_fids[2] && g_cache.move_key_fids[3];
}

}

bool ensure_cache(JNIEnv* env)
{
    if (!env) return false;
    if (g_cache.initialized) return true;
    std::scoped_lock lk(s_cache_mx);
    if (g_cache.initialized) return true;

    g_cache.minecraft_class = cache_class(env, "net/minecraft/class_310");
    if (!g_cache.minecraft_class) return false;

    g_cache.entity_class        = cache_class(env, "net/minecraft/class_1297");
    g_cache.living_class        = cache_class(env, "net/minecraft/class_1309");
    g_cache.player_class        = cache_class(env, "net/minecraft/class_1657");
    g_cache.client_player_class = cache_class(env, "net/minecraft/class_746");
    g_cache.armor_stand_class   = cache_class(env, "net/minecraft/class_1531");
    g_cache.world_class         = cache_class(env, "net/minecraft/class_638");
    g_cache.vec3d_class         = cache_class(env, "net/minecraft/class_243");
    g_cache.box_class           = cache_class(env, "net/minecraft/class_238");
    g_cache.inventory_class     = cache_class(env, "net/minecraft/class_1661");
    g_cache.item_stack_class    = cache_class(env, "net/minecraft/class_1799");
    g_cache.item_class          = cache_class(env, "net/minecraft/class_1792");
    g_cache.options_class       = cache_class(env, "net/minecraft/class_315");
    g_cache.simple_option_class = cache_class(env, "net/minecraft/class_7172");
    g_cache.render_tick_counter_class = cache_class(env, "net/minecraft/class_9779");
    g_cache.block_hit_class     = cache_class(env, "net/minecraft/class_3965");
    g_cache.key_binding_class   = cache_class(env, "net/minecraft/class_304");

    g_cache.mc_instance_fid = env->GetStaticFieldID(g_cache.minecraft_class,
                                                    "field_1700", "Lnet/minecraft/class_310;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_cache.mc_instance_fid)
        g_cache.mc_instance_fid = env->GetStaticFieldID(g_cache.minecraft_class,
                                                        "instance", "Lnet/minecraft/class_310;");
    if (env->ExceptionCheck()) env->ExceptionClear();

    g_cache.mc_player_fid    = cache_field_either(env, g_cache.minecraft_class, "field_1724", "player",   "Lnet/minecraft/class_746;");
    g_cache.mc_world_fid     = cache_field_either(env, g_cache.minecraft_class, "field_1687", "level",    "Lnet/minecraft/class_638;");
    g_cache.mc_options_fid   = cache_field_either(env, g_cache.minecraft_class, "field_1690", "options",  "Lnet/minecraft/class_315;");
    g_cache.mc_screen_fid    = cache_field_either(env, g_cache.minecraft_class, "field_1755", "screen",   "Lnet/minecraft/class_437;");
    g_cache.mc_render_tick_counter_mid = cache_method_either(env, g_cache.minecraft_class,
        "method_61966", "getTimer", "()Lnet/minecraft/class_9779;");
    if (g_cache.render_tick_counter_class)
        g_cache.rtc_tick_progress_mid = cache_method_either(env, g_cache.render_tick_counter_class,
            "method_60637", "getGameTimeDeltaPartialTick", "(Z)F");
    g_cache.mc_crosshair_fid = cache_field_either(env, g_cache.minecraft_class, "field_1765", "hitResult","Lnet/minecraft/class_239;");

    if (g_cache.options_class)
        g_cache.opts_sens_fid = cache_field_either(env, g_cache.options_class,
                                                   "field_1843", "mouseSensitivity",
                                                   "Lnet/minecraft/class_7172;");
    if (g_cache.simple_option_class)
        g_cache.sopt_get_value_mid = cache_method_either(env, g_cache.simple_option_class,
                                                         "method_41753", "get",
                                                         "()Ljava/lang/Object;");

    if (g_cache.entity_class) {
        g_cache.ent_get_x_mid = cache_method_either(env, g_cache.entity_class, "method_23317", "getX", "()D");
        g_cache.ent_get_y_mid = cache_method_either(env, g_cache.entity_class, "method_23318", "getY", "()D");
        g_cache.ent_get_z_mid = cache_method_either(env, g_cache.entity_class, "method_23321", "getZ", "()D");
        g_cache.ent_is_alive_mid   = cache_method_either(env, g_cache.entity_class, "method_5805",  "isAlive",     "()Z");
        g_cache.ent_on_ground_mid  = cache_method_either(env, g_cache.entity_class, "method_24828", "onGround",    "()Z");
        g_cache.ent_invisible_mid  = cache_method_either(env, g_cache.entity_class, "method_5767",  "isInvisible", "()Z");
        g_cache.ent_eye_mid        = cache_method_either(env, g_cache.entity_class, "method_5751",  "getEyeHeight","()F");
        g_cache.ent_yaw_fid        = cache_field_either(env, g_cache.entity_class, "field_6031", "yRot", "F");
        g_cache.ent_pitch_fid      = cache_field_either(env, g_cache.entity_class, "field_5965", "xRot", "F");
        g_cache.ent_prev_yaw_fid   = cache_field_either(env, g_cache.entity_class, "field_5982", "yRotO", "F");
        g_cache.ent_prev_pitch_fid = cache_field_either(env, g_cache.entity_class, "field_6004", "xRotO", "F");
        g_cache.ent_velocity_fid   = cache_field_either(env, g_cache.entity_class, "field_18276", "deltaMovement", "Lnet/minecraft/class_243;");
        g_cache.ent_prev_x_fid     = cache_field_either(env, g_cache.entity_class, "field_6014", "xo", "D");
        g_cache.ent_prev_y_fid     = cache_field_either(env, g_cache.entity_class, "field_6036", "yo", "D");
        g_cache.ent_prev_z_fid     = cache_field_either(env, g_cache.entity_class, "field_5969", "zo", "D");
        g_cache.ent_bbox_fid       = cache_field_either(env, g_cache.entity_class, "field_6005", "bb", "Lnet/minecraft/class_238;");
        g_cache.ent_bbox_mid       = cache_method_either(env, g_cache.entity_class, "method_5829", "getBoundingBox", "()Lnet/minecraft/class_238;");
    }

    if (g_cache.box_class) {
        g_cache.box_min_x_fid = cache_field_either(env, g_cache.box_class, "field_1323", "minX", "D");
        g_cache.box_min_y_fid = cache_field_either(env, g_cache.box_class, "field_1322", "minY", "D");
        g_cache.box_min_z_fid = cache_field_either(env, g_cache.box_class, "field_1321", "minZ", "D");
        g_cache.box_max_x_fid = cache_field_either(env, g_cache.box_class, "field_1320", "maxX", "D");
        g_cache.box_max_y_fid = cache_field_either(env, g_cache.box_class, "field_1325", "maxY", "D");
        g_cache.box_max_z_fid = cache_field_either(env, g_cache.box_class, "field_1324", "maxZ", "D");
    }

    if (g_cache.client_player_class) {
        g_cache.cp_render_yaw_fid        = cache_field(env, g_cache.client_player_class, "field_3932", "F");
        g_cache.cp_render_pitch_fid      = cache_field(env, g_cache.client_player_class, "field_3916", "F");
        g_cache.cp_last_render_yaw_fid   = cache_field(env, g_cache.client_player_class, "field_3931", "F");
        g_cache.cp_last_render_pitch_fid = cache_field(env, g_cache.client_player_class, "field_3914", "F");
    }

    if (g_cache.vec3d_class) {
        g_cache.vec_x_fid = cache_field_either(env, g_cache.vec3d_class, "field_1352", "x", "D");
        g_cache.vec_y_fid = cache_field_either(env, g_cache.vec3d_class, "field_1351", "y", "D");
        g_cache.vec_z_fid = cache_field_either(env, g_cache.vec3d_class, "field_1350", "z", "D");
    }

    if (g_cache.living_class) {
        g_cache.living_health_mid   = cache_method_either(env, g_cache.living_class, "method_6032", "getHealth", "()F");
        g_cache.living_armor_mid    = cache_method_either(env, g_cache.living_class, "method_6096", "getArmor",  "()I");
        g_cache.living_mainhand_mid = cache_method_either(env, g_cache.living_class, "method_6047", "getMainHandStack",
                                                          "()Lnet/minecraft/class_1799;");
    }

    if (g_cache.player_class)
        g_cache.player_inventory_fid = cache_field_either(env, g_cache.player_class,
                                                          "field_7514", "inventory",
                                                          "Lnet/minecraft/class_1661;");
    if (g_cache.inventory_class) {
        g_cache.inv_selected_fid  = cache_field_either(env, g_cache.inventory_class, "field_7545", "selected", "I");
        g_cache.inv_get_stack_mid = cache_method_either(env, g_cache.inventory_class, "method_5438", "getItem",
                                                        "(I)Lnet/minecraft/class_1799;");
    }
    if (g_cache.item_stack_class) {
        g_cache.stack_get_item_mid = cache_method_either(env, g_cache.item_stack_class, "method_7909", "getItem",
                                                         "()Lnet/minecraft/class_1792;");
        g_cache.stack_is_empty_mid = cache_method_either(env, g_cache.item_stack_class, "method_7960", "isEmpty", "()Z");
    }
    if (g_cache.item_class)
        g_cache.item_get_key_mid = cache_method_either(env, g_cache.item_class, "method_7876", "getDescriptionId",
                                                       "()Ljava/lang/String;");

    if (g_cache.world_class)
        g_cache.world_entities_mid = cache_method_either(env, g_cache.world_class,
                                                         "method_18112", "entitiesForRendering",
                                                         "()Ljava/lang/Iterable;");

    {
        jclass iterable = env->FindClass("java/lang/Iterable");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (iterable) {
            g_cache.iterable_iterator_mid = env->GetMethodID(iterable, "iterator", "()Ljava/util/Iterator;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(iterable);
        }
        jclass iterator = env->FindClass("java/util/Iterator");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (iterator) {
            g_cache.iterator_has_next_mid = env->GetMethodID(iterator, "hasNext", "()Z");
            g_cache.iterator_next_mid     = env->GetMethodID(iterator, "next", "()Ljava/lang/Object;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(iterator);
        }
        jclass jdouble = env->FindClass("java/lang/Double");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (jdouble) {
            g_cache.double_value_mid = env->GetMethodID(jdouble, "doubleValue", "()D");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(jdouble);
        }
    }

    if (g_cache.key_binding_class) {
        g_cache.key_translation_mid = cache_method_either(env, g_cache.key_binding_class,
                                                          "method_1431", "getName",
                                                          "()Ljava/lang/String;");
        g_cache.key_pressed_fid = find_sole_boolean_field(env, g_cache.key_binding_class);
    }

    g_cache.initialized = true;

    char r[256];
    std::snprintf(r, sizeof(r),
        "pos=%d%d%d lastPos=%d%d%d bbox=%d box=%d vel=%d yaw=%d pitch=%d "
        "lastYaw=%d lastPitch=%d sway=%d%d sens=%d getValue=%d screen=%d crosshair=%d "
        "onGround=%d alive=%d health=%d entities=%d",
        g_cache.ent_get_x_mid ? 1 : 0, g_cache.ent_get_y_mid ? 1 : 0,
        g_cache.ent_get_z_mid ? 1 : 0,
        g_cache.ent_prev_x_fid ? 1 : 0, g_cache.ent_prev_y_fid ? 1 : 0,
        g_cache.ent_prev_z_fid ? 1 : 0,
        (g_cache.ent_bbox_fid || g_cache.ent_bbox_mid) ? 1 : 0,
        g_cache.box_min_x_fid ? 1 : 0,
        g_cache.ent_velocity_fid ? 1 : 0,
        g_cache.ent_yaw_fid ? 1 : 0, g_cache.ent_pitch_fid ? 1 : 0,
        g_cache.ent_prev_yaw_fid ? 1 : 0, g_cache.ent_prev_pitch_fid ? 1 : 0,
        g_cache.cp_render_yaw_fid ? 1 : 0, g_cache.cp_render_pitch_fid ? 1 : 0,
        g_cache.opts_sens_fid ? 1 : 0, g_cache.sopt_get_value_mid ? 1 : 0,
        g_cache.mc_screen_fid ? 1 : 0, g_cache.mc_crosshair_fid ? 1 : 0,
        g_cache.ent_on_ground_mid ? 1 : 0, g_cache.ent_is_alive_mid ? 1 : 0,
        g_cache.living_health_mid ? 1 : 0, g_cache.world_entities_mid ? 1 : 0);
    bool valid = g_cache.minecraft_class && g_cache.mc_player_fid && g_cache.mc_world_fid;
    g_cache.initialized = valid;

    VLOG_INFO("aimassist", "vape port cache: {} (valid={})", r, valid ? 1 : 0);
    debug::resolveLine(r);
    return valid;
}

const JniCache& cache() { return g_cache; }

void drop_cache(JNIEnv* env)
{
    if (!env) return;
    std::scoped_lock lk(s_cache_mx);
    auto drop = [&](jclass& c) { if (c) { env->DeleteGlobalRef(c); c = nullptr; } };
    drop(g_cache.minecraft_class);
    drop(g_cache.entity_class);
    drop(g_cache.living_class);
    drop(g_cache.player_class);
    drop(g_cache.client_player_class);
    drop(g_cache.armor_stand_class);
    drop(g_cache.world_class);
    drop(g_cache.vec3d_class);
    drop(g_cache.box_class);
    drop(g_cache.inventory_class);
    drop(g_cache.item_stack_class);
    drop(g_cache.item_class);
    drop(g_cache.options_class);
    drop(g_cache.simple_option_class);
    drop(g_cache.block_hit_class);
    drop(g_cache.key_binding_class);
    g_cache.initialized = false;
}

EntPos entityPos(JNIEnv* env, jobject ent)
{
    const JniCache& c = cache();
    EntPos p{0, 0, 0};
    if (!ent || !c.ent_get_x_mid) return p;
    p.x = env->CallDoubleMethod(ent, c.ent_get_x_mid);
    p.y = ent == nullptr ? 0 : env->CallDoubleMethod(ent, c.ent_get_y_mid);
    p.z = ent == nullptr ? 0 : env->CallDoubleMethod(ent, c.ent_get_z_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return EntPos{0, 0, 0}; }
    return p;
}

EntPos entityPrevPos(JNIEnv* env, jobject ent)
{
    const JniCache& c = cache();
    EntPos p{0, 0, 0};
    if (!ent || !c.ent_prev_x_fid) return p;
    p.x = env->GetDoubleField(ent, c.ent_prev_x_fid);
    p.y = env->GetDoubleField(ent, c.ent_prev_y_fid);
    p.z = env->GetDoubleField(ent, c.ent_prev_z_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return EntPos{0, 0, 0}; }
    return p;
}

float renderTickDelta(JNIEnv* env)
{
    const JniCache& c = cache();
    if (!c.mc_instance_fid || !c.mc_render_tick_counter_mid || !c.rtc_tick_progress_mid)
        return 1.0f;
    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return 1.0f; }
    jobject rtc = env->CallObjectMethod(mc, c.mc_render_tick_counter_mid);
    env->DeleteLocalRef(mc);
    if (env->ExceptionCheck() || !rtc) { env->ExceptionClear(); return 1.0f; }
    float td = env->CallFloatMethod(rtc, c.rtc_tick_progress_mid, JNI_TRUE);
    env->DeleteLocalRef(rtc);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 1.0f; }
    if (!(td >= 0.f)) td = 0.f;
    if (td > 1.f) td = 1.f;
    return td;
}

EntPos entityRenderPos(JNIEnv* env, jobject ent, float td)
{
    const EntPos cur = entityPos(env, ent);
    const JniCache& c = cache();
    if (!ent || !c.ent_prev_x_fid) return cur;
    const EntPos prev = entityPrevPos(env, ent);
    const double dx = cur.x - prev.x, dy = cur.y - prev.y, dz = cur.z - prev.z;

    if (dx * dx + dy * dy + dz * dz > 2.25) return cur;
    return EntPos{ prev.x + dx * static_cast<double>(td),
                   prev.y + dy * static_cast<double>(td),
                   prev.z + dz * static_cast<double>(td) };
}

EntVel entityVelocity(JNIEnv* env, jobject ent)
{
    const JniCache& c = cache();
    EntVel v{0, 0, 0};
    if (!ent || !c.ent_velocity_fid) return v;
    jobject vec = env->GetObjectField(ent, c.ent_velocity_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return v; }
    if (!vec || !c.vec_x_fid) return v;
    v.x = env->GetDoubleField(vec, c.vec_x_fid);
    v.y = env->GetDoubleField(vec, c.vec_y_fid);
    v.z = env->GetDoubleField(vec, c.vec_z_fid);
    env->DeleteLocalRef(vec);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return EntVel{0, 0, 0}; }
    return v;
}

float entityEyeHeight(JNIEnv* env, jobject ent)
{
    const JniCache& c = cache();
    if (!ent || !c.ent_eye_mid) return 1.62f;
    const float h = env->CallFloatMethod(ent, c.ent_eye_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 1.62f; }
    return h;
}

Box6 entityBox(JNIEnv* env, jobject ent)
{
    const JniCache& c = cache();
    Box6 b{0, 0, 0, 0, 0, 0, false};
    if (!ent || !c.box_min_x_fid) return b;

    jobject box = nullptr;
    if (c.ent_bbox_fid) box = env->GetObjectField(ent, c.ent_bbox_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); box = nullptr; }
    if (!box && c.ent_bbox_mid) {
        box = env->CallObjectMethod(ent, c.ent_bbox_mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); box = nullptr; }
    }
    if (!box) return b;

    b.minX = env->GetDoubleField(box, c.box_min_x_fid);
    b.minY = env->GetDoubleField(box, c.box_min_y_fid);
    b.minZ = env->GetDoubleField(box, c.box_min_z_fid);
    b.maxX = env->GetDoubleField(box, c.box_max_x_fid);
    b.maxY = env->GetDoubleField(box, c.box_max_y_fid);
    b.maxZ = env->GetDoubleField(box, c.box_max_z_fid);
    env->DeleteLocalRef(box);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return Box6{0,0,0,0,0,0,false}; }
    b.valid = (b.maxX > b.minX && b.maxY > b.minY && b.maxZ > b.minZ);
    return b;
}

float playerSensitivity(JNIEnv* env)
{
    const JniCache& c = cache();
    if (!env || !c.mc_instance_fid || !c.mc_options_fid || !c.opts_sens_fid
        || !c.sopt_get_value_mid || !c.double_value_mid)
        return 0.5f;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0.5f; }
    if (!mc) return 0.5f;
    jobject opts = env->GetObjectField(mc, c.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(mc); return 0.5f; }
    if (!opts) { env->DeleteLocalRef(mc); return 0.5f; }
    jobject sopt = env->GetObjectField(opts, c.opts_sens_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    env->DeleteLocalRef(opts);
    env->DeleteLocalRef(mc);
    if (!sopt) return 0.5f;

    jobject boxed = env->CallObjectMethod(sopt, c.sopt_get_value_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); boxed = nullptr; }
    env->DeleteLocalRef(sopt);
    if (!boxed) return 0.5f;
    const double v = env->CallDoubleMethod(boxed, c.double_value_mid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    env->DeleteLocalRef(boxed);
    return static_cast<float>(v);
}

float strafeInput(JNIEnv* env, jobject mc)
{
    const JniCache& c = cache();
    if (!env || !mc) return 0.f;
    resolve_movement_keys(env, mc);
    if (!c.move_keys_resolved || !c.mc_options_fid || !c.key_pressed_fid) return 0.f;

    jobject options = env->GetObjectField(mc, c.mc_options_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0.f; }
    if (!options) return 0.f;

    auto pressed = [&](int idx) -> int {
        jobject kb = env->GetObjectField(options, c.move_key_fids[idx]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
        if (!kb) return 0;
        const jboolean p = env->GetBooleanField(kb, c.key_pressed_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(kb);
        return p == JNI_TRUE ? 1 : 0;
    };

    const float s = static_cast<float>(pressed(2) - pressed(3));
    env->DeleteLocalRef(options);
    return s;
}

void applyTrackedMouseDelta(JNIEnv* env, jobject player, float yawDelta, float pitchDelta)
{
    const JniCache& c = cache();
    if (!env || !player || !c.ent_yaw_fid || !c.ent_pitch_fid) return;

    const float trackedYaw   = env->GetFloatField(player, c.ent_yaw_fid);
    const float trackedPitch = env->GetFloatField(player, c.ent_pitch_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    float newYaw   = trackedYaw   + yawDelta   * 0.15f;
    float newPitch = trackedPitch - pitchDelta * 0.15f;
    if (newPitch < -90.f) newPitch = -90.f;
    if (newPitch >  90.f) newPitch =  90.f;

    env->SetFloatField(player, c.ent_yaw_fid,   newYaw);
    env->SetFloatField(player, c.ent_pitch_fid, newPitch);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    if (c.ent_prev_yaw_fid) {
        env->SetFloatField(player, c.ent_prev_yaw_fid,
                           env->GetFloatField(player, c.ent_prev_yaw_fid)
                               + (newYaw - trackedYaw));
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (c.ent_prev_pitch_fid) {
        env->SetFloatField(player, c.ent_prev_pitch_fid,
                           env->GetFloatField(player, c.ent_prev_pitch_fid)
                               + (newPitch - trackedPitch));
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

JavaRandom::JavaRandom()
{
    setSeed(static_cast<uint64_t>(nowNanos()) ^ 0x5DEECE6DULL);
}

void JavaRandom::setSeed(uint64_t s)
{
    seed48 = (s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
}

int JavaRandom::next(int bits)
{
    seed48 = (seed48 * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);

    return static_cast<int>(static_cast<uint32_t>(seed48 >> (48 - bits)));
}

int JavaRandom::nextInt()
{
    return next(32);
}

int JavaRandom::nextInt(int bound)
{
    if (bound <= 0) return 0;
    if ((bound & -bound) == bound)
        return static_cast<int>((static_cast<uint64_t>(bound) * static_cast<uint32_t>(next(31))) >> 31);
    int bits, val;
    do {
        bits = next(31);
        val = bits % bound;
    } while (bits - val + (bound - 1) < 0);
    return val;
}

float JavaRandom::nextFloat()
{
    return static_cast<float>(next(24)) / static_cast<float>(1 << 24);
}

double JavaRandom::nextDouble()
{
    return ((static_cast<uint64_t>(static_cast<uint32_t>(next(26))) << 27)
            + static_cast<uint32_t>(next(27))) * (1.0 / 9007199254740992.0);
}

float wrapAngleTo180_f(float f)
{
    f = std::fmod(f, 360.f);
    if (f >= 180.f) f -= 360.f;
    if (f < -180.f) f += 360.f;
    return f;
}

double wrapAngleTo180_d(double d)
{
    d = std::fmod(d, 360.0);
    if (d >= 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return d;
}

static double quadrantYawTo(double dx, double dz)
{
    constexpr double kDeg = 180.0 / 3.14159265358979323846;
    double yaw = 0.0;
    if (dz > 0.0 && dx > 0.0)      yaw = -(std::atan(dx / dz) * kDeg);
    else if (dz > 0.0 && dx < 0.0) yaw = -(std::atan(dx / dz) * kDeg);
    else if (dz < 0.0 && dx > 0.0) yaw = -90.0 + (std::atan(dz / dx) * kDeg);
    else if (dz < 0.0 && dx < 0.0) yaw =  90.0 + (std::atan(dz / dx) * kDeg);
    return yaw;
}

float rotationYawTo(double px, double pz, double tx, double tz)
{
    return static_cast<float>(quadrantYawTo(tx - px, tz - pz));
}

int angleToTarget(float playerYaw, double px, double pz, double tx, double tz)
{

    const double yaw = quadrantYawTo(tx - px, tz - pz);
    const int n = static_cast<int>(std::fmod(std::fabs(yaw - static_cast<double>(playerYaw)), 360.0));
    return n > 180 ? 360 - n : n;
}

double horizAngleDiff(double px, double pz, float yaw, double tx, double tz)
{

    const double yawTo = quadrantYawTo(tx - px, tz - pz);
    const double d9 = std::fmod(std::fabs(yawTo - static_cast<double>(yaw)), 360.0);
    return d9 > 180.0 ? 360.0 - d9 : d9;
}

bool targetOnLeft(double px, double pz, float yaw, double tx, double tz)
{

    const double yawTo = quadrantYawTo(tx - px, tz - pz);
    const int n = static_cast<int>(wrapAngleTo180_d(std::fmod(yawTo - static_cast<double>(yaw), 360.0)));
    return n < 0;
}

int vertAngleDiff(float playerPitch, double px, double py, double pz,
                  double aimX, double aimY, double aimZ)
{

    const double dx = aimX - px;
    const double dy = aimY - py;
    const double dz = aimZ - pz;
    const double horiz = std::sqrt(dx * dx + dz * dz);
    const float f = static_cast<float>(-(std::atan2(dy, horiz) * 180.0 / 3.14159265358979323846));
    return static_cast<int>(wrapAngleTo180_d(static_cast<double>(playerPitch) - f));
}

double clampToRange(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

Vec3 closestPointOnBox(double ex, double ey, double ez, const Box6& box)
{

    double x = clampToRange(ex, box.minX, box.maxX);
    const double y = clampToRange(ey, box.minY, box.maxY);
    double z = clampToRange(ez, box.minZ, box.maxZ);
    if (x == ex) x = ex + 0.01;
    if (z == ez) z = ez + 0.01;
    return Vec3{x, y, z};
}

Vec3 stableClosestPointOnBox(double ex, double ey, double ez, const Box6& box)
{

    const double y = clampToRange(ey, box.minY, box.maxY);
    const bool insideX = (ex >= box.minX && ex <= box.maxX);
    const bool insideZ = (ez >= box.minZ && ez <= box.maxZ);
    if (insideX && insideZ) {

        return Vec3{(box.minX + box.maxX) * 0.5, y,
                    (box.minZ + box.maxZ) * 0.5};
    }

    const double x = clampToRange(ex, box.minX, box.maxX);
    const double z = clampToRange(ez, box.minZ, box.maxZ);
    return Vec3{x, y, z};
}

int drainSmoothed(float& state, float pendingUnits, float smoothSetting, float dtSec)
{

    if (smoothSetting <= 0.01f || dtSec <= 0.f) {
        const int raw = static_cast<int>(pendingUnits);
        state = 0.f;
        return raw;
    }

    if (std::fabs(pendingUnits) < 2.f) {
        const int raw = static_cast<int>(pendingUnits);
        state = 0.f;
        return raw;
    }

    const float k = std::max(8.f, 48.f - smoothSetting * 4.f);
    const float rawRate = pendingUnits / dtSec;
    const float a = 1.f - std::exp(-k * dtSec);
    state += (rawRate - state) * a;
    float allowAbs = std::fabs(state * dtSec);

    if (allowAbs > 90.f) allowAbs = 90.f;
    const float mag = std::fabs(pendingUnits) < allowAbs
                    ? std::fabs(pendingUnits) : allowAbs;
    return static_cast<int>(mag) * (pendingUnits < 0.f ? -1 : 1);
}

double randomRange(JavaRandom& rnd, double lo, double hi)
{
    return lo + (hi - lo) * rnd.nextDouble();
}

int randomExclusiveUpper(JavaRandom& rnd, int lo, int hi)
{
    return rnd.nextInt(hi - lo) + lo;
}

}
