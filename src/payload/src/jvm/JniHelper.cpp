#include "jvm/JniHelper.h"

#include "util/Hash.h"
#include "util/Log.h"

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace dt::jvm {

namespace {

struct ClassCacheEntry {
    jclass ref = nullptr; // global
};
struct MemberKey {
    std::uint64_t owner;
    std::uint64_t name;
    bool          isStatic;
    friend bool operator==(MemberKey a, MemberKey b) noexcept {
        return a.owner == b.owner && a.name == b.name && a.isStatic == b.isStatic;
    }
};
struct MemberKeyHasher {
    using is_avalanching = void;
    std::size_t operator()(MemberKey k) const noexcept {
        auto h = k.owner ^ (k.name * 0x9E3779B97F4A7C15ull) ^ (k.isStatic ? 1u : 0u);
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
        return static_cast<std::size_t>(h);
    }
};

// L1 Hot Lock-Free Cache sizes (power of two for fast bitwise indexing)
constexpr std::size_t HOT_MEMBER_CACHE_SIZE = 1024;
constexpr std::size_t HOT_CLASS_CACHE_SIZE  = 512;

struct alignas(32) HotMemberSlot {
    std::atomic<std::uint64_t> owner{0};
    std::atomic<std::uint64_t> name{0};
    std::atomic<std::uint32_t> flags{0}; // bit 0: isStatic, bit 1: valid
    std::atomic<void*>         ptr{nullptr};
};

struct alignas(16) HotClassSlot {
    std::atomic<std::uint64_t> key{0};
    std::atomic<jclass>        ref{nullptr}; // global reference
};

HotMemberSlot g_hotMethods[HOT_MEMBER_CACHE_SIZE];
HotMemberSlot g_hotFields[HOT_MEMBER_CACHE_SIZE];
HotClassSlot  g_hotClasses[HOT_CLASS_CACHE_SIZE];
HotClassSlot  g_hotRuntimeClasses[HOT_CLASS_CACHE_SIZE];

std::shared_mutex                                                                 g_rwMx;
struct RuntimeClassEntry {
    jclass ref = nullptr; // global; callers receive a fresh LOCAL reference
    std::chrono::steady_clock::time_point retry{};
};
ankerl::unordered_dense::map<std::uint64_t, RuntimeClassEntry>                    g_runtimeClasses;
ankerl::unordered_dense::map<std::uint64_t, std::chrono::steady_clock::time_point> g_canonRetry;
ankerl::unordered_dense::map<std::uint64_t, ClassCacheEntry>                      g_classCache;
ankerl::unordered_dense::map<MemberKey, jmethodID, MemberKeyHasher>               g_methodCache;
ankerl::unordered_dense::map<MemberKey, jfieldID,  MemberKeyHasher>               g_fieldCache;

// Classes we have already complained about, so the retry loop stays quiet.
ankerl::unordered_dense::set<std::uint64_t>                                       g_warnedMissing;

/* @findLoadedClass:
	the class-loader-agnostic lookup.

	JVMTI's GetLoadedClasses reports every class the VM has loaded, whichever
	loader did it, so matching on the signature finds Minecraft's classes under
	Fabric's Knot loader where FindClass cannot. Expensive — thousands of
	classes, a signature allocation each — but findClassCanon caches the result,
	so it runs once per class for the life of the process.
*/
jclass findLoadedClass(JNIEnv* env, const std::string& binaryName) {
    // Silent: this is a fallback, and the caller reports the overall failure
    // once per class rather than once per attempt.
    jvmtiEnv* ti = ctx().jvmti.load();
    if (!ti) return nullptr;

    const std::string wanted = "L" + binaryName + ";";

    jint    count   = 0;
    jclass* classes = nullptr;
    if (ti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE || !classes) return nullptr;

    jclass found = nullptr;

    for (jint i = 0; i < count; ++i) {
        if (!found) {
            char* sig = nullptr;
            if (ti->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && sig) {
                if (wanted == sig) found = classes[i];
                ti->Deallocate(reinterpret_cast<unsigned char*>(sig));
            }
        }
        // Every entry is a local reference; the ones we are not keeping have to
        // go, or a single scan floods the local reference table.
        if (classes[i] != found) env->DeleteLocalRef(classes[i]);
    }

    ti->Deallocate(reinterpret_cast<unsigned char*>(classes));
    return found;
}

} // namespace

bool checkAndClearException(JNIEnv* env, std::string_view scope);

namespace {

/* @findViaContextClassLoader:
	the lookup that needs no JVMTI at all.

	Fabric runs the game from a thread whose CONTEXT class loader is Knot, and a
	class loader will happily load a class by name for anyone holding it. So:
	walk to the root thread group, enumerate every live thread, and ask each
	one's context loader for the class. The first that answers is the game's.

	This exists because JVMTI is not guaranteed. Dynamic agent loading is
	deprecated on JDK 21 and refused outright by default from JDK 24, and this
	path keeps working when GetEnv(JVMTI) is denied.
*/
jclass findViaContextClassLoader(JNIEnv* env, const std::string& binaryName) {
    // loadClass takes a dotted name, not the slashed binary form
    std::string dotted = binaryName;
    for (char& c : dotted) if (c == '/') c = '.';

    jclass threadCls = env->FindClass("java/lang/Thread");
    jclass groupCls  = env->FindClass("java/lang/ThreadGroup");
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (!threadCls || !groupCls || !loaderCls) { checkAndClearException(env, "ctxLoader/bootstrap"); return nullptr; }

    jmethodID currentThread = env->GetStaticMethodID(threadCls, "currentThread", "()Ljava/lang/Thread;");
    jmethodID getGroup      = env->GetMethodID(threadCls, "getThreadGroup", "()Ljava/lang/ThreadGroup;");
    jmethodID getCtxLoader  = env->GetMethodID(threadCls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID getParent     = env->GetMethodID(groupCls, "getParent", "()Ljava/lang/ThreadGroup;");
    jmethodID activeCount   = env->GetMethodID(groupCls, "activeCount", "()I");
    jmethodID enumerate     = env->GetMethodID(groupCls, "enumerate", "([Ljava/lang/Thread;)I");
    jmethodID loadClass     = env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    if (!currentThread || !getGroup || !getCtxLoader || !getParent || !activeCount || !enumerate || !loadClass) {
        checkAndClearException(env, "ctxLoader/methods");
        return nullptr;
    }

    jobject self = env->CallStaticObjectMethod(threadCls, currentThread);
    if (!self) { checkAndClearException(env, "ctxLoader/self"); return nullptr; }

    jobject group = env->CallObjectMethod(self, getGroup);
    env->DeleteLocalRef(self);
    if (!group) { checkAndClearException(env, "ctxLoader/group"); return nullptr; }

    // climb to the root group so enumerate() sees every thread in the VM
    for (int depth = 0; depth < 16; ++depth) {
        jobject parent = env->CallObjectMethod(group, getParent);
        if (!parent) break;
        env->DeleteLocalRef(group);
        group = parent;
    }

    const jint count = env->CallIntMethod(group, activeCount) + 16;   // headroom for races
    jobjectArray threads = env->NewObjectArray(count, threadCls, nullptr);
    if (!threads) { env->DeleteLocalRef(group); return nullptr; }

    const jint found = env->CallIntMethod(group, enumerate, threads);
    env->DeleteLocalRef(group);
    checkAndClearException(env, "ctxLoader/enumerate");

    jstring name = env->NewStringUTF(dotted.c_str());
    jclass  result = nullptr;

    for (jint i = 0; i < found && !result; ++i) {
        jobject t = env->GetObjectArrayElement(threads, i);
        if (!t) continue;

        jobject loader = env->CallObjectMethod(t, getCtxLoader);
        env->DeleteLocalRef(t);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;

        jobject cls = env->CallObjectMethod(loader, loadClass, name);
        env->DeleteLocalRef(loader);

        // A loader that does not know the class throws; that is the normal
        // answer for most threads and must not be treated as a failure.
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (cls) result = static_cast<jclass>(cls);
    }

    env->DeleteLocalRef(name);
    env->DeleteLocalRef(threads);

    return result;
}

} // namespace

jclass findClassAnyLoader(JNIEnv* env, const char* binaryName) {
    if (!env || !binaryName) return nullptr;
    const std::uint64_t key = fnv1a64(binaryName);
    const std::size_t slotIdx = key & (HOT_CLASS_CACHE_SIZE - 1);

    auto& slot = g_hotRuntimeClasses[slotIdx];
    // 1. L1 Hot Slot fast path (returns fresh NewLocalRef from cached global ref with zero locks)
    if (slot.key.load(std::memory_order_acquire) == key) {
        jclass globalRef = slot.ref.load(std::memory_order_relaxed);
        if (globalRef) {
            return static_cast<jclass>(env->NewLocalRef(globalRef));
        }
    }

    // 2. L2 Concurrent Read Cache
    const auto now = std::chrono::steady_clock::now();
    {
        std::shared_lock rlock(g_rwMx);
        auto it = g_runtimeClasses.find(key);
        if (it != g_runtimeClasses.end()) {
            if (it->second.ref) {
                jclass globalRef = it->second.ref;
                slot.ref.store(globalRef, std::memory_order_relaxed);
                slot.key.store(key, std::memory_order_release);
                return static_cast<jclass>(env->NewLocalRef(globalRef));
            }
            if (now < it->second.retry) return nullptr;
        }
    }

    // Cold path: update retry throttle under write lock
    {
        std::unique_lock wlock(g_rwMx);
        auto& cached = g_runtimeClasses[key];
        if (cached.ref) {
            jclass globalRef = cached.ref;
            slot.ref.store(globalRef, std::memory_order_relaxed);
            slot.key.store(key, std::memory_order_release);
            return static_cast<jclass>(env->NewLocalRef(globalRef));
        }
        if (now < cached.retry) return nullptr;
        cached.retry = now + std::chrono::milliseconds(500);
    }

    const std::string name(binaryName);
    jclass c = env->FindClass(binaryName);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!c) c = findViaContextClassLoader(env, name);
    if (!c) c = findLoadedClass(env, name);
    if (c) {
        auto global = static_cast<jclass>(env->NewGlobalRef(c));
        if (global) {
            std::unique_lock wlock(g_rwMx);
            auto& cached = g_runtimeClasses[key];
            if (!cached.ref) {
                cached.ref = global;
            } else {
                env->DeleteGlobalRef(global);
                global = cached.ref;
            }
            slot.ref.store(global, std::memory_order_relaxed);
            slot.key.store(key, std::memory_order_release);
        }
    }
    return c;
}

bool checkAndClearException(JNIEnv* env, std::string_view scope) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    VLOG_WARN("jni", "Pending exception cleared in scope='{}'", scope);
    return true;
}

jclass findClassCanon(JNIEnv* env, std::string_view canon) {
    if (!env) return nullptr;
    const auto key = fnv1a64(canon);
    const std::size_t slotIdx = key & (HOT_CLASS_CACHE_SIZE - 1);

    auto& slot = g_hotClasses[slotIdx];
    // 1. L1 Hot Slot fast path (lock-free global ref return)
    if (slot.key.load(std::memory_order_acquire) == key) {
        jclass ref = slot.ref.load(std::memory_order_relaxed);
        if (ref) return ref;
    }

    // 2. L2 Concurrent Read Cache
    const auto now = std::chrono::steady_clock::now();
    {
        std::shared_lock rlock(g_rwMx);
        auto it = g_classCache.find(key);
        if (it != g_classCache.end() && it->second.ref) {
            jclass ref = it->second.ref;
            slot.ref.store(ref, std::memory_order_relaxed);
            slot.key.store(key, std::memory_order_release);
            return ref;
        }
        auto retryIt = g_canonRetry.find(key);
        if (retryIt != g_canonRetry.end() && now < retryIt->second) return nullptr;
    }

    // Cold path: throttle retries under write lock
    {
        std::unique_lock wlock(g_rwMx);
        auto it = g_classCache.find(key);
        if (it != g_classCache.end() && it->second.ref) {
            jclass ref = it->second.ref;
            slot.ref.store(ref, std::memory_order_relaxed);
            slot.key.store(key, std::memory_order_release);
            return ref;
        }
        g_canonRetry[key] = now + std::chrono::milliseconds(500);
    }

    auto runtimeName = mappings::MappingRegistry::get().classFor(canon);
    if (runtimeName.empty()) {
        VLOG_DEBUG("jni", "classFor('{}') -> unresolved on this version", canon);
        return nullptr;
    }

    std::string zeroTerminated(runtimeName);
    jclass local = env->FindClass(zeroTerminated.c_str());
    /* Cleared silently, on purpose. Under Fabric a miss here is the EXPECTED
       outcome, not a fault worth reporting — routing it through
       checkAndClearException printed a stack trace and a warning on every
       single polling attempt. */
    if (env->ExceptionCheck()) env->ExceptionClear();

    /* FindClass is expected to fail here, and that is not an error.

       On a thread WE attached, FindClass resolves through the SYSTEM class
       loader. Fabric does not put Minecraft on it: Knot loads the game in a
       loader of its own, so every net/minecraft/* lookup from a native thread
       comes back null with a ClassNotFoundException. Asking the VM which
       classes are actually loaded sidesteps loaders entirely. */
    if (!local) {
        // Ask the loaders that actually have it. Context loaders first: that
        // path needs no JVMTI, so it keeps working where JVMTI is refused.
        local = findViaContextClassLoader(env, zeroTerminated);
        if (!local) local = findLoadedClass(env, zeroTerminated);

        if (!local) {
            /* Once per class, not once per attempt. Callers poll while the game
               is still loading, and three lines every 500 ms buried everything
               else in the log — including the lines that explain why. */
            bool first = false;
            {
                std::unique_lock wlock(g_rwMx);
                first = g_warnedMissing.insert(key).second;
            }
            if (first) {
                VLOG_WARN("jni", "class '{}' ({}) not reachable yet — retrying quietly",
                          canon, runtimeName);
            }
            return nullptr;
        }

        VLOG_INFO("jni", "resolved '{}' through the game's class loader", canon);
    }

    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);

    {
        std::unique_lock wlock(g_rwMx);
        auto it = g_classCache.find(key);
        if (it != g_classCache.end() && it->second.ref) {
            env->DeleteGlobalRef(global);
            jclass existing = it->second.ref;
            slot.ref.store(existing, std::memory_order_relaxed);
            slot.key.store(key, std::memory_order_release);
            return existing;
        }
        g_classCache[key] = ClassCacheEntry{global};
    }

    slot.ref.store(global, std::memory_order_relaxed);
    slot.key.store(key, std::memory_order_release);
    return global;
}

jmethodID methodCanon(JNIEnv* env, std::string_view owner, std::string_view method, bool isStatic) {
    if (!env) return nullptr;

    const std::uint64_t ownerHash = fnv1a64(owner);
    const std::uint64_t methodHash = fnv1a64(method);
    const std::uint32_t expectedFlags = (isStatic ? 1u : 0u) | 2u;
    const MemberKey k{ownerHash, methodHash, isStatic};
    const std::size_t slotIdx = MemberKeyHasher{}(k) & (HOT_MEMBER_CACHE_SIZE - 1);

    auto& slot = g_hotMethods[slotIdx];
    // 1. L1 Lock-free Hot Cache fast path (~1-2 ns)
    if (slot.flags.load(std::memory_order_acquire) == expectedFlags &&
        slot.owner.load(std::memory_order_relaxed) == ownerHash &&
        slot.name.load(std::memory_order_relaxed) == methodHash) {
        auto id = reinterpret_cast<jmethodID>(slot.ptr.load(std::memory_order_relaxed));
        if (id) return id;
    }

    // 2. L2 Concurrent Read Cache
    {
        std::shared_lock rlock(g_rwMx);
        auto it = g_methodCache.find(k);
        if (it != g_methodCache.end()) {
            jmethodID id = it->second;
            slot.flags.store(0, std::memory_order_relaxed);
            slot.owner.store(ownerHash, std::memory_order_relaxed);
            slot.name.store(methodHash, std::memory_order_relaxed);
            slot.ptr.store(reinterpret_cast<void*>(id), std::memory_order_relaxed);
            slot.flags.store(expectedFlags, std::memory_order_release);
            return id;
        }
    }

    // 3. L3 Cold Resolution
    jclass cls = findClassCanon(env, owner);
    if (!cls) return nullptr;
    auto ref = mappings::MappingRegistry::get().methodFor(owner, method);
    if (ref.name.empty()) {
        VLOG_DEBUG("jni", "methodFor('{}','{}') unresolved", owner, method);
        return nullptr;
    }
    std::string name(ref.name), desc(ref.desc);
    jmethodID id = isStatic ? env->GetStaticMethodID(cls, name.c_str(), desc.c_str())
                            : env->GetMethodID(cls, name.c_str(), desc.c_str());
    checkAndClearException(env, "GetMethodID");
    if (!id) return nullptr;

    {
        std::unique_lock wlock(g_rwMx);
        g_methodCache[k] = id;
    }

    slot.flags.store(0, std::memory_order_relaxed);
    slot.owner.store(ownerHash, std::memory_order_relaxed);
    slot.name.store(methodHash, std::memory_order_relaxed);
    slot.ptr.store(reinterpret_cast<void*>(id), std::memory_order_relaxed);
    slot.flags.store(expectedFlags, std::memory_order_release);
    return id;
}

jfieldID fieldCanon(JNIEnv* env, std::string_view owner, std::string_view field, bool isStatic) {
    if (!env) return nullptr;

    const std::uint64_t ownerHash = fnv1a64(owner);
    const std::uint64_t fieldHash = fnv1a64(field);
    const std::uint32_t expectedFlags = (isStatic ? 1u : 0u) | 2u;
    const MemberKey k{ownerHash, fieldHash, isStatic};
    const std::size_t slotIdx = MemberKeyHasher{}(k) & (HOT_MEMBER_CACHE_SIZE - 1);

    auto& slot = g_hotFields[slotIdx];
    // 1. L1 Lock-free Hot Cache fast path (~1-2 ns)
    if (slot.flags.load(std::memory_order_acquire) == expectedFlags &&
        slot.owner.load(std::memory_order_relaxed) == ownerHash &&
        slot.name.load(std::memory_order_relaxed) == fieldHash) {
        auto id = reinterpret_cast<jfieldID>(slot.ptr.load(std::memory_order_relaxed));
        if (id) return id;
    }

    // 2. L2 Concurrent Read Cache
    {
        std::shared_lock rlock(g_rwMx);
        auto it = g_fieldCache.find(k);
        if (it != g_fieldCache.end()) {
            jfieldID id = it->second;
            slot.flags.store(0, std::memory_order_relaxed);
            slot.owner.store(ownerHash, std::memory_order_relaxed);
            slot.name.store(fieldHash, std::memory_order_relaxed);
            slot.ptr.store(reinterpret_cast<void*>(id), std::memory_order_relaxed);
            slot.flags.store(expectedFlags, std::memory_order_release);
            return id;
        }
    }

    // 3. L3 Cold Resolution
    jclass cls = findClassCanon(env, owner);
    if (!cls) return nullptr;
    auto ref = mappings::MappingRegistry::get().fieldFor(owner, field);
    if (ref.name.empty()) {
        VLOG_DEBUG("jni", "fieldFor('{}','{}') unresolved", owner, field);
        return nullptr;
    }
    std::string name(ref.name), desc(ref.desc);
    jfieldID id = isStatic ? env->GetStaticFieldID(cls, name.c_str(), desc.c_str())
                           : env->GetFieldID(cls, name.c_str(), desc.c_str());
    checkAndClearException(env, "GetFieldID");
    if (!id) return nullptr;

    {
        std::unique_lock wlock(g_rwMx);
        g_fieldCache[k] = id;
    }

    slot.flags.store(0, std::memory_order_relaxed);
    slot.owner.store(ownerHash, std::memory_order_relaxed);
    slot.name.store(fieldHash, std::memory_order_relaxed);
    slot.ptr.store(reinterpret_cast<void*>(id), std::memory_order_relaxed);
    slot.flags.store(expectedFlags, std::memory_order_release);
    return id;
}

void invalidateCaches() {
    JNIEnv* env = thisThreadEnv();
    std::unique_lock wlock(g_rwMx);

    for (auto& s : g_hotMethods) {
        s.flags.store(0, std::memory_order_release);
        s.owner.store(0, std::memory_order_relaxed);
        s.name.store(0, std::memory_order_relaxed);
        s.ptr.store(nullptr, std::memory_order_relaxed);
    }
    for (auto& s : g_hotFields) {
        s.flags.store(0, std::memory_order_release);
        s.owner.store(0, std::memory_order_relaxed);
        s.name.store(0, std::memory_order_relaxed);
        s.ptr.store(nullptr, std::memory_order_relaxed);
    }
    for (auto& s : g_hotClasses) {
        s.key.store(0, std::memory_order_release);
        s.ref.store(nullptr, std::memory_order_relaxed);
    }
    for (auto& s : g_hotRuntimeClasses) {
        s.key.store(0, std::memory_order_release);
        s.ref.store(nullptr, std::memory_order_relaxed);
    }

    if (env) {
        for (auto& [k, entry] : g_classCache) {
            if (entry.ref) {
                env->DeleteGlobalRef(entry.ref);
            }
        }
        for (auto& [k, cached] : g_runtimeClasses) {
            if (cached.ref) {
                env->DeleteGlobalRef(cached.ref);
            }
        }
    }
    g_classCache.clear();
    g_runtimeClasses.clear();
    g_canonRetry.clear();
    g_methodCache.clear();
    g_fieldCache.clear();
    g_warnedMissing.clear();
}

// ── Call helpers ─────────────────────────────────────────────────────────────
LocalObject callObjectCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method) {
    jmethodID id = methodCanon(env, owner, method);
    if (!id) return {};
    jobject o = env->CallObjectMethod(self, id);
    checkAndClearException(env, "CallObjectMethod");
    return {env, o};
}
jint callIntCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method) {
    jmethodID id = methodCanon(env, owner, method);
    if (!id) return 0;
    jint v = env->CallIntMethod(self, id);
    checkAndClearException(env, "CallIntMethod");
    return v;
}
jfloat callFloatCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method) {
    jmethodID id = methodCanon(env, owner, method);
    if (!id) return 0.f;
    jfloat v = env->CallFloatMethod(self, id);
    checkAndClearException(env, "CallFloatMethod");
    return v;
}
jboolean callBoolCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method) {
    jmethodID id = methodCanon(env, owner, method);
    if (!id) return JNI_FALSE;
    jboolean v = env->CallBooleanMethod(self, id);
    checkAndClearException(env, "CallBooleanMethod");
    return v;
}
void callVoidCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view method) {
    jmethodID id = methodCanon(env, owner, method);
    if (!id) return;
    env->CallVoidMethod(self, id);
    checkAndClearException(env, "CallVoidMethod");
}

LocalObject staticCallObjectCanon(JNIEnv* env, std::string_view owner, std::string_view method) {
    jclass    cls = findClassCanon(env, owner);
    jmethodID id  = methodCanon(env, owner, method, /*isStatic=*/true);
    if (!cls || !id) return {};
    jobject o = env->CallStaticObjectMethod(cls, id);
    checkAndClearException(env, "CallStaticObjectMethod");
    return {env, o};
}

jobject getObjectFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field) {
    jfieldID id = fieldCanon(env, owner, field);
    return id ? env->GetObjectField(self, id) : nullptr;
}
jfloat getFloatFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field) {
    jfieldID id = fieldCanon(env, owner, field);
    return id ? env->GetFloatField(self, id) : 0.f;
}
jdouble getDoubleFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field) {
    jfieldID id = fieldCanon(env, owner, field);
    return id ? env->GetDoubleField(self, id) : 0.0;
}
jint getIntFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field) {
    jfieldID id = fieldCanon(env, owner, field);
    return id ? env->GetIntField(self, id) : 0;
}
jboolean getBoolFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field) {
    jfieldID id = fieldCanon(env, owner, field);
    return id ? env->GetBooleanField(self, id) : JNI_FALSE;
}

void setFloatFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field, jfloat v) {
    if (auto id = fieldCanon(env, owner, field)) env->SetFloatField(self, id, v);
}
void setDoubleFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field, jdouble v) {
    if (auto id = fieldCanon(env, owner, field)) env->SetDoubleField(self, id, v);
}
void setIntFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field, jint v) {
    if (auto id = fieldCanon(env, owner, field)) env->SetIntField(self, id, v);
}
void setBoolFieldCanon(JNIEnv* env, jobject self, std::string_view owner, std::string_view field, jboolean v) {
    if (auto id = fieldCanon(env, owner, field)) env->SetBooleanField(self, id, v);
}

} // namespace dt::jvm
