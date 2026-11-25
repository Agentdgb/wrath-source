#include <jnihook.h>
#include "core/NativeCallbacks.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "classfile.hpp"
#include "uuid.hpp"

// Hook::ensure_tls_for_this_thread — wirea el TLS block del PE para
// el thread actual. Se llama al inicio de cada callback JVMTI porque
// el JVM puede invocar el callback desde threads que el OS / JVM
// crearon DESPUÉS de la inyección, y para los cuales el inyector NO
// wireó TLS. Sin esta llamada, cualquier acceso a STL (unordered_map,
// string) crashea con AV en gs:[58] + tls_index*8.
namespace Hook { void ensure_tls_for_this_thread(); }

typedef struct jnihook_t {
        JavaVM   *jvm;
        jvmtiEnv *jvmti;
} jnihook_t;

typedef struct method_info_t {
        std::string name;
        std::string signature;
        jint access_flags;
} method_info_t;

typedef struct hook_info_t {
        method_info_t method_info;
        void *native_hook_method;
} hook_info_t;

static std::unique_ptr<jnihook_t> g_jnihook = nullptr;
static std::unordered_map<std::string, std::vector<hook_info_t>> g_hooks;
static std::unordered_map<std::string, std::unique_ptr<ClassFile>> g_class_file_cache;
static std::unordered_map<std::string, jclass> g_original_classes;
static std::unordered_map<std::string, jclass> g_declaring_classes;

static std::string
get_class_signature(jvmtiEnv *jvmti, jclass clazz)
{
        char *sig;

        if (jvmti->GetClassSignature(clazz, &sig, NULL) != JVMTI_ERROR_NONE) {
                return "";
        }

        std::string signature = std::string(sig, &sig[strlen(sig)]);

        jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

        return signature;
}

static std::string
get_class_name(JNIEnv *env, jclass clazz)
{
        jclass klass = env->FindClass("java/lang/Class");
        if (!klass)
                return "";

        jmethodID getName_method = env->GetMethodID(klass, "getName", "()Ljava/lang/String;");
        if (!getName_method)
                return "";

        jstring name_obj = reinterpret_cast<jstring>(env->CallObjectMethod(clazz, getName_method));
        if (!name_obj)
                return "";

        const char *c_name = env->GetStringUTFChars(name_obj, 0);
        if (!c_name)
                return "";

        std::string name = std::string(c_name, &c_name[strlen(c_name)]);

        env->ReleaseStringUTFChars(name_obj, c_name);

        for (size_t i = 0; i < name.length(); ++i) {
                if (name[i] == '.')
                        name[i] = '/';
        }

        return name;
}

static std::unique_ptr<method_info_t>
get_method_info(jvmtiEnv *jvmti, jmethodID method)
{
        char *name;
        char *sig;
        jint access_flags;

        if (jvmti->GetMethodName(method, &name, &sig, NULL) != JVMTI_ERROR_NONE)
                return nullptr;

        if (jvmti->GetMethodModifiers(method, &access_flags) != JVMTI_ERROR_NONE)
                return nullptr;

        std::string name_str(name, &name[strlen(name)]);
        std::string signature_str(sig, &sig[strlen(sig)]);

        jvmti->Deallocate(reinterpret_cast<unsigned char *>(name));
        jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

        return std::make_unique<method_info_t>(method_info_t { name_str, signature_str, access_flags });
}

void JNICALL JNIHook_ClassFileLoadHook(jvmtiEnv *jvmti_env,
                                       JNIEnv* jni_env,
                                       jclass class_being_redefined,
                                       jobject loader,
                                       const char* name,
                                       jobject protection_domain,
                                       jint class_data_len,
                                       const unsigned char* class_data,
                                       jint* new_class_data_len,
                                       unsigned char** new_class_data)
{
        // CRÍTICO: el JVM invoca este callback desde threads arbitrarios
        // (suspend/resume, ClassLoader workers, etc). Si el inyector no
        // wireó el TLS para ese thread, cualquier toque de STL hace AV.
        Hook::ensure_tls_for_this_thread();

        // DT PATCH — JVMTI passes class_being_redefined == NULL for an
        // ORDINARY class load; it is non-null only for a redefine/retransform.
        // get_class_name() does CallObjectMethod(clazz, ...) with no null check,
        // so every normal class load that happened while this callback was armed
        // was calling into JNI with a null receiver. Nothing to do for a plain
        // load anyway — we only ever rewrite classes we are retransforming.
        if (!class_being_redefined)
                return;

        auto class_name = get_class_name(jni_env, class_being_redefined);

        if (class_name == "" || g_hooks.find(class_name) == g_hooks.end() || g_hooks[class_name].size() == 0)
                return;

        if (g_class_file_cache.find(class_name) == g_class_file_cache.end()) {
                auto cf = ClassFile::load(class_data);
                if (!cf)
                        return;

                g_class_file_cache[class_name] = std::move(cf);
        }

        return;
}

jnihook_result_t
ReapplyClass(jclass clazz, std::string clazz_name)
{
        jvmtiClassDefinition class_definition;

        auto cf = *g_class_file_cache[clazz_name];

        auto constant_pool = cf.get_constant_pool();

        for (auto &method : cf.get_methods()) {
                auto name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                        cf.get_constant_pool_item(method.name_index).bytes.data()
                );

                auto descriptor_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                        cf.get_constant_pool_item(method.descriptor_index).bytes.data()
                );

                auto name = std::string(name_ci->bytes, &name_ci->bytes[name_ci->length]);
                auto descriptor = std::string(descriptor_ci->bytes, &descriptor_ci->bytes[descriptor_ci->length]);

                bool should_hook = false;
                for (auto &hk_info : g_hooks[clazz_name]) {
                        auto &minfo = hk_info.method_info;
                        if (minfo.name == name && minfo.signature == descriptor) {
                                should_hook = true;
                                break;
                        }
                }
                if (!should_hook)
                        continue;

                method.access_flags |= ACC_NATIVE;

                for (size_t i = 0; i < method.attributes.size(); ++i) {
                        auto attr = method.attributes[i];
                        auto attr_name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                                cf.get_constant_pool_item(attr.attribute_name_index).bytes.data()
                        );
                        auto attr_name = std::string(attr_name_ci->bytes, &attr_name_ci->bytes[attr_name_ci->length]);
                        if (attr_name == "Code") {
                                method.attributes.erase(method.attributes.begin() + i);
                                break;
                        }
                }
        }

        auto cf_bytes = cf.bytes();

        class_definition.klass = clazz;
        class_definition.class_byte_count = cf_bytes.size();
        class_definition.class_bytes = cf_bytes.data();
        if (g_jnihook->jvmti->RedefineClasses(1, &class_definition) != JVMTI_ERROR_NONE)
                return JNIHOOK_ERR_JVMTI_OPERATION;

        return JNIHOOK_OK;
}

static jnihook_result_t
JNIHook_InitInternal(JavaVM *jvm, jvmtiEnv *jvmti, bool add_caps)
{
        if (g_jnihook) return g_jnihook->jvm == jvm ? JNIHOOK_OK : JNIHOOK_ERR_GET_JVMTI;
        jvmtiCapabilities potential = {};
        jvmtiCapabilities capabilities = {};
        jvmtiEventCallbacks callbacks = {};

        if (jvmti->GetPotentialCapabilities(&potential) != JVMTI_ERROR_NONE) {
                return JNIHOOK_ERR_ADD_JVMTI_CAPS;
        }

        capabilities.can_redefine_classes = potential.can_redefine_classes;
        capabilities.can_redefine_any_class = potential.can_redefine_any_class;
        capabilities.can_suspend = potential.can_suspend;
        if (add_caps) {
                if (!potential.can_retransform_classes) {
                        return JNIHOOK_ERR_ADD_JVMTI_CAPS;
                }
                capabilities.can_retransform_classes = 1;
                capabilities.can_retransform_any_class = potential.can_retransform_any_class;
        }

        if (jvmti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
                return JNIHOOK_ERR_ADD_JVMTI_CAPS;
        }

        callbacks.ClassFileLoadHook = JNIHook_ClassFileLoadHook;
        if (jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
                return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
        }

        g_jnihook = std::make_unique<jnihook_t>(jnihook_t { jvm, jvmti });

        return JNIHOOK_OK;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Init(JavaVM *jvm)
{
        if (!jvm) return JNIHOOK_ERR_GET_JNI;
        jvmtiEnv *jvmti = nullptr;
        jint res = jvm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION);
        if (res != JNI_OK) {
            res = jvm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION_1_2);
        }
        if (res != JNI_OK) {
            return JNIHOOK_ERR_GET_JVMTI;
        }
        return JNIHook_InitInternal(jvm, jvmti, true);
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_InitWithEnv(JavaVM *jvm, jvmtiEnv *jvmti)
{
        return JNIHook_InitInternal(jvm, jvmti, false);
}

static jnihook_result_t
AttachInternal(jmethodID method, void *native_hook_method, jmethodID *original_method,
               std::atomic<jmethodID> *atomic_original)
{
        jclass clazz;
        std::string clazz_name;
        hook_info_t hook_info;
        jobject class_loader;
        JNIEnv *env;

        if (!g_jnihook) {
                return JNIHOOK_ERR_GET_JVMTI;
        }

        if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
                return JNIHOOK_ERR_GET_JNI;
        }

        if (g_jnihook->jvmti->GetMethodDeclaringClass(method, &clazz) != JVMTI_ERROR_NONE) {
                return JNIHOOK_ERR_JVMTI_OPERATION;
        }

        clazz_name = get_class_name(env, clazz);
        if (clazz_name.length() == 0) {
                return JNIHOOK_ERR_JNI_OPERATION;
        }
        if (!g_declaring_classes.contains(clazz_name)) {
                auto retained = static_cast<jclass>(env->NewGlobalRef(clazz));
                if (!retained) return JNIHOOK_ERR_JNI_OPERATION;
                g_declaring_classes.emplace(clazz_name, retained);
        }

        auto method_info = get_method_info(g_jnihook->jvmti, method);
        if (!method_info) {
                return JNIHOOK_ERR_JVMTI_OPERATION;
        }

        hook_info.method_info = *method_info;
        hook_info.native_hook_method = native_hook_method;

        if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
                if (g_jnihook->jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
                        return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
                }

                g_hooks[clazz_name].push_back(hook_info);
                auto result = g_jnihook->jvmti->RetransformClasses(1, &clazz);
                g_hooks[clazz_name].pop_back();

                if (g_jnihook->jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
                        return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
                }

                if (result != JVMTI_ERROR_NONE)
                        return JNIHOOK_ERR_CLASS_FILE_CACHE;

                if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
                        return JNIHOOK_ERR_CLASS_FILE_CACHE;
                }
        }

        if (g_original_classes.find(clazz_name) == g_original_classes.end()) {
                std::string class_copy_name = clazz_name + "_" + GenerateUuid();
                std::string class_shortname = class_copy_name.substr(class_copy_name.find_last_of('/') + 1);
                std::string class_copy_source_name = class_shortname + ".java";
                jclass class_copy;
                auto cf = *g_class_file_cache[clazz_name];

                for (auto &attr : cf.get_attributes()) {
                        auto attr_name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                                cf.get_constant_pool_item(attr.attribute_name_index).bytes.data()
                        );
                        auto attr_name = std::string(attr_name_ci->bytes, &attr_name_ci->bytes[attr_name_ci->length]);
                        if (attr_name != "SourceFile")
                                continue;

                        u2 attr_index_be = ((attr.attribute_name_index >> 8) & 0xff) |
                                           ((attr.attribute_name_index & 0xff) << 8);
                        u2 source = *reinterpret_cast<u2 *>(attr.info.data());

                        if (source == attr_index_be)
                                break;

                        CONSTANT_Utf8_info ci;
                        cp_info sourcefile_cpi;
                        ci.tag = CONSTANT_Utf8;
                        ci.length = static_cast<u2>(class_copy_source_name.size());

                        sourcefile_cpi.bytes = std::vector<uint8_t>(sizeof(ci) + ci.length);
                        memcpy(sourcefile_cpi.bytes.data(), &ci, sizeof(ci));
                        memcpy(&sourcefile_cpi.bytes.data()[sizeof(ci)], class_copy_source_name.c_str(), ci.length);

                        cf.set_constant_pool_item_be(source, sourcefile_cpi);
                }

                for (auto &cpi : cf.get_constant_pool()) {
                        if (cpi.bytes[0] != CONSTANT_Class)
                                continue;

                        auto class_ci = reinterpret_cast<CONSTANT_Class_info *>(
                                cpi.bytes.data()
                        );

                        auto name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                                cf.get_constant_pool_item(class_ci->name_index).bytes.data()
                        );

                        auto name = std::string(name_ci->bytes, &name_ci->bytes[name_ci->length]);

                        if (name == clazz_name) {

                                CONSTANT_Utf8_info ci;
                                cp_info cpi;

                                ci.tag = CONSTANT_Utf8;
                                ci.length = static_cast<u2>(class_copy_name.size());

                                cpi.bytes = std::vector<uint8_t>(sizeof(ci) + ci.length);
                                memcpy(cpi.bytes.data(), &ci, sizeof(ci));
                                memcpy(&cpi.bytes.data()[sizeof(ci)], class_copy_name.c_str(), ci.length);

                                cf.set_constant_pool_item(class_ci->name_index, cpi);
                                break;
                        }
                }

                auto constant_pool = cf.get_constant_pool();
                for (auto &item : constant_pool) {
                        if (item.bytes[0] != CONSTANT_NameAndType)
                                continue;

                        auto nt_ci = reinterpret_cast<CONSTANT_NameAndType_info *>(item.bytes.data());
                        auto descriptor_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
                                cf.get_constant_pool_item(nt_ci->descriptor_index).bytes.data()
                        );
                        auto descriptor = std::string(descriptor_ci->bytes, &descriptor_ci->bytes[descriptor_ci->length]);

                        std::string clazz_desc = std::string("L") + clazz_name + ";";
                        std::string clazz_copy_desc = std::string("L") + class_copy_name + ";";
                        if (auto index = descriptor.find(clazz_desc); index != descriptor.npos) {

                                CONSTANT_Utf8_info ci;
                                cp_info cpi;
                                std::string new_descriptor = descriptor.replace(index, clazz_desc.size(), clazz_copy_desc);

                                ci.tag = CONSTANT_Utf8;
                                ci.length = static_cast<u2>(new_descriptor.size());

                                cpi.bytes = std::vector<uint8_t>(sizeof(ci) + ci.length);
                                memcpy(cpi.bytes.data(), &ci, sizeof(ci));
                                memcpy(&cpi.bytes.data()[sizeof(ci)], new_descriptor.c_str(), ci.length);

                                cf.set_constant_pool_item(nt_ci->descriptor_index, cpi);
                        }
                }

                auto methods = cf.get_methods();
                for (auto& method : methods)
                {
                        auto descriptor_index = method.descriptor_index;

                        auto descriptor_ci = reinterpret_cast<CONSTANT_Utf8_info*>(
                                cf.get_constant_pool_item(descriptor_index).bytes.data()
                                );

                        auto descriptor = std::string(descriptor_ci->bytes, &descriptor_ci->bytes[descriptor_ci->length]);

                        std::string clazz_desc = std::string("L") + clazz_name + ";";
                        std::string clazz_copy_desc = std::string("L") + class_copy_name + ";";

                        for (size_t index; (index = descriptor.find(clazz_desc)) != descriptor.npos;)
                        {
                                CONSTANT_Utf8_info ci;
                                cp_info cpi;
                                std::string new_descriptor = descriptor.replace(index, clazz_desc.size(), clazz_copy_desc);

                                ci.tag = CONSTANT_Utf8;
                                ci.length = static_cast<u2>(new_descriptor.size());

                                cpi.bytes = std::vector<uint8_t>(sizeof(ci) + ci.length);
                                memcpy(cpi.bytes.data(), &ci, sizeof(ci));
                                memcpy(&cpi.bytes.data()[sizeof(ci)], new_descriptor.c_str(), ci.length);

                                cf.set_constant_pool_item(descriptor_index, cpi);
                        }
                }

                auto class_data = cf.bytes();

                if (g_jnihook->jvmti->GetClassLoader(clazz, &class_loader) != JVMTI_ERROR_NONE)
                        return JNIHOOK_ERR_JVMTI_OPERATION;

                class_copy = env->DefineClass(NULL, class_loader,
                                              reinterpret_cast<const jbyte *>(class_data.data()),
                                              class_data.size());

                if (!class_copy)
                        return JNIHOOK_ERR_JNI_OPERATION;

                // DT PATCH — must be a GLOBAL ref.
                //
                // DefineClass() above returns a LOCAL reference, valid only until
                // the current JNI frame pops. Storing it in this process-lifetime
                // map left a dangling jclass behind: the FIRST attach on a class
                // works (the ref is still live in the same frame), but any LATER
                // attach on the SAME class takes the cache-hit path below, reads
                // this dead reference and calls GetMethodID on it — undefined
                // behaviour, and in practice a garbage/failed original method.
                //
                // That is exactly how a second hook on class_310 (autototem's
                // MinecraftClient.tick, added alongside the triggerbot's
                // doAttack) produced an unusable original and froze the client.
                g_original_classes[clazz_name] = static_cast<jclass>(env->NewGlobalRef(class_copy));
                env->DeleteLocalRef(class_copy);
                if (!g_original_classes[clazz_name]) {
                        g_original_classes.erase(clazz_name);
                        return JNIHOOK_ERR_JNI_OPERATION;
                }
        }

        if (g_original_classes.find(clazz_name) == g_original_classes.end()) {
                return JNIHOOK_ERR_CLASS_FILE_CACHE;
        }

        if (original_method || atomic_original) {
                jclass orig_class = g_original_classes[clazz_name];
                jmethodID orig;

                if ((method_info->access_flags & ACC_STATIC) == ACC_STATIC) {
                        orig = env->GetStaticMethodID(orig_class, method_info->name.c_str(),
                                                      method_info->signature.c_str());
                } else {
                        orig = env->GetMethodID(orig_class, method_info->name.c_str(),
                                                method_info->signature.c_str());
                }

                if (!orig || env->ExceptionOccurred()) {
                        env->ExceptionClear();
                        return JNIHOOK_ERR_JAVA_EXCEPTION;
                }

                if (original_method) *original_method = orig;
                if (atomic_original) atomic_original->store(orig, std::memory_order_release);
        }

        jthread curthread;
        jthread *threads;
        jint thread_count;

        env->PushLocalFrame(16);

        if (g_jnihook->jvmti->GetCurrentThread(&curthread) != JVMTI_ERROR_NONE)
                return JNIHOOK_ERR_JVMTI_OPERATION;

        if (g_jnihook->jvmti->GetAllThreads(&thread_count, &threads) != JVMTI_ERROR_NONE)
                return JNIHOOK_ERR_JVMTI_OPERATION;

        for (jint i = 0; i < thread_count; ++i) {
                if (env->IsSameObject(threads[i], curthread))
                        continue;

                g_jnihook->jvmti->SuspendThread(threads[i]);
        }

        jnihook_result_t ret;
        g_hooks[clazz_name].push_back(hook_info);
        if (ret = ReapplyClass(clazz, clazz_name); ret != JNIHOOK_OK) {
                g_hooks[clazz_name].pop_back();
                goto RESUME_THREADS;
        }

        JNINativeMethod native_method;
        native_method.name = const_cast<char *>(method_info->name.c_str());
        native_method.signature = const_cast<char *>(method_info->signature.c_str());
        native_method.fnPtr = native_hook_method;

        if (env->RegisterNatives(clazz, &native_method, 1) < 0) {
                g_hooks[clazz_name].pop_back();
                ReapplyClass(clazz, clazz_name);
                ret = JNIHOOK_ERR_JNI_OPERATION;
                goto RESUME_THREADS;
        }

        ret = JNIHOOK_OK;

RESUME_THREADS:

        for (jint i = 0; i < thread_count; ++i) {
                if (env->IsSameObject(threads[i], curthread))
                        continue;

                g_jnihook->jvmti->ResumeThread(threads[i]);
        }

        g_jnihook->jvmti->Deallocate(reinterpret_cast<unsigned char *>(threads));
        env->PopLocalFrame(NULL);

        return ret;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Attach(jmethodID method, void *native_hook_method, jmethodID *original_method)
{
        return AttachInternal(method, native_hook_method, original_method, nullptr);
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_AttachAtomic(jmethodID method, void *native_hook_method,
                     std::atomic<jmethodID> *original_method)
{
        return AttachInternal(method, native_hook_method, nullptr, original_method);
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Detach(jmethodID method)
{
        JNIEnv *env;
        jclass clazz;
        std::string clazz_name;
        hook_info_t hook_info;
        jvmtiClassDefinition class_definition;

        if (!g_jnihook) {
                return JNIHOOK_OK;
        }

        if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
                return JNIHOOK_ERR_GET_JNI;
        }

        if (g_jnihook->jvmti->GetMethodDeclaringClass(method, &clazz) != JVMTI_ERROR_NONE) {
                return JNIHOOK_ERR_JVMTI_OPERATION;
        }

        clazz_name = get_class_name(env, clazz);
        if (clazz_name.length() == 0) {
                return JNIHOOK_ERR_JNI_OPERATION;
        }

        if (g_hooks.find(clazz_name) == g_hooks.end() || g_hooks[clazz_name].size() == 0) {
                return JNIHOOK_OK;
        }

        auto method_info = get_method_info(g_jnihook->jvmti, method);
        if (!method_info) {
                return JNIHOOK_ERR_JVMTI_OPERATION;
        }

        auto saved = g_hooks[clazz_name];
        std::erase_if(g_hooks[clazz_name], [&](const auto &h) {
                return h.method_info.name == method_info->name &&
                       h.method_info.signature == method_info->signature;
        });

        auto ret = ReapplyClass(clazz, clazz_name);
        if (ret != JNIHOOK_OK) g_hooks[clazz_name] = std::move(saved);
        // An obsolete native frame may still call its original. Retain the
        // cloned classes and bytecode until Shutdown drains all native frames.
        return ret;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Shutdown()
{

        if (!g_jnihook) {
                g_hooks.clear();
                g_class_file_cache.clear();
                g_original_classes.clear();
                g_declaring_classes.clear();
                return JNIHOOK_OK;
        }

        JNIEnv *env;
        jvmtiEventCallbacks callbacks = {};

        if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
                return JNIHOOK_ERR_GET_JNI;
        }

        // B-2 step 3: drain frames for 50 ms before unpatching.
        Sleep(50);

        jnihook_result_t first_error = JNIHOOK_OK;
        for (auto &[key, _value] : g_class_file_cache) {
                const auto found = g_declaring_classes.find(key);
                if (found == g_declaring_classes.end() || !found->second) {
                        if (first_error == JNIHOOK_OK) first_error = JNIHOOK_ERR_JNI_OPERATION;
                        continue;
                }
                auto saved = std::move(g_hooks[key]);
                g_hooks[key].clear();

                // B-2 steps 4-6: RetransformClasses per class, logging exact jvmtiError.
                if (g_jnihook->jvmti->SetEventNotificationMode(
                        JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
                        g_hooks[key] = std::move(saved);
                        if (first_error == JNIHOOK_OK) first_error = JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
                        continue;
                }
                const jvmtiError rt_err = g_jnihook->jvmti->RetransformClasses(1, &found->second);
                g_jnihook->jvmti->SetEventNotificationMode(
                        JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);

                char dbg[192]{};
                std::snprintf(dbg, sizeof(dbg), "[jnihook] RetransformClasses(%s) jvmtiError=%d\n",
                              key.c_str(), static_cast<int>(rt_err));
                OutputDebugStringA(dbg);

                if (rt_err == JVMTI_ERROR_MUST_POSSESS_CAPABILITY) {
                        g_hooks[key] = std::move(saved);
                        return JNIHOOK_ERR_MUST_POSSESS_CAPABILITY;
                }
                if (rt_err == JVMTI_ERROR_UNMODIFIABLE_CLASS) {
                        // Non-modifiable (hidden/lambda): note and continue with remaining classes.
                        continue;
                }
                if (rt_err != JVMTI_ERROR_NONE) {
                        g_hooks[key] = std::move(saved);
                        if (first_error == JNIHOOK_OK) first_error = JNIHOOK_ERR_JVMTI_OPERATION;
                }
        }

        if (!dt::drainNativeCallbacks() && first_error == JNIHOOK_OK)
                first_error = JNIHOOK_ERR_JVMTI_OPERATION;
        if (first_error != JNIHOOK_OK) return first_error;

        // Keep all caches/references until every restoration and lifecycle step
        // succeeds. A later call can retry a partial shutdown safely.
        const auto disabled = g_jnihook->jvmti->SetEventNotificationMode(
            JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        const auto cleared = g_jnihook->jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));
        if (disabled != JVMTI_ERROR_NONE || cleared != JVMTI_ERROR_NONE)
                return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
        if (g_jnihook->jvmti->DisposeEnvironment() != JVMTI_ERROR_NONE)
                return JNIHOOK_ERR_JVMTI_OPERATION;

        for (auto& [name, clazz] : g_original_classes) if (clazz) env->DeleteGlobalRef(clazz);
        g_original_classes.clear();
        for (auto& [name, clazz] : g_declaring_classes) if (clazz) env->DeleteGlobalRef(clazz);
        g_declaring_classes.clear();

        g_hooks.clear();
        g_class_file_cache.clear();

        g_jnihook = nullptr;

        return JNIHOOK_OK;
}
