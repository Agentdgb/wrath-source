#include "jvm/JvmContext.h"

#include "platform/Win32Process.h"
#include "core/UnloadSafety.h"
#include "util/Log.h"
#include "shared/strings/mc_names.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <windows.h>

namespace vanta::jvm {

namespace {

JvmContext              g_ctx{};
std::once_flag          g_initOnce;

using JNI_GetCreatedJavaVMs_t = jint (JNICALL*)(JavaVM**, jsize, jsize*);

/* Every call into jvm.dll gets its own SEH wrapper.

   A single guard around the whole bring-up produced one opaque "SEH
   0xC0000005" that named no call, and this cannot be reproduced without a real
   Minecraft to inject into — so the code has to say where it died. MSVC forbids
   __try in any function holding an object with a destructor, which is why each
   of these is a bare leaf taking out-params. */

bool sehGetCreatedVMs(JNI_GetCreatedJavaVMs_t p, JavaVM** vm, jsize* count,
                      jint* rc, DWORD* fault) {
    __try {
        *rc = p(vm, 1, count);
        *fault = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return false;
    }
}

bool sehGetEnv(JavaVM* vm, void** out, jint version, jint* rc, DWORD* fault) {
    __try {
        *rc = vm->GetEnv(out, version);
        *fault = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return false;
    }
}

void hex8(DWORD v, char* out) { std::snprintf(out, 16, "%08lX", v); }

} // namespace

JvmContext& ctx() noexcept { return g_ctx; }

void init(JavaVM* vm) {
    std::call_once(g_initOnce, [vm] {
        // The VM pointer is what everything else needs. Publish it BEFORE the
        // JVMTI request, so a failure there still leaves plain JNI usable —
        // reading the account name needs no JVMTI at all.
        g_ctx.vm.store(vm);
        VLOG_INFO("jvm", "JavaVM* acquired at {}", reinterpret_cast<std::uintptr_t>(vm));

        /* ATTACH FIRST. This is not housekeeping, it is the precondition.

           HotSpot serves GetEnv(JVMTI_VERSION) through
           JvmtiExport::get_jvmti_interface, which calls JavaThread::current().
           On a thread the VM has never seen there is no JavaThread, and the
           call either hands back nothing or dereferences null — which is where
           the 0xC0000005 during bring-up came from. Attaching first gives the
           VM a thread to answer about. */
        if (!thisThreadEnv()) {
            VLOG_ERR("jvm", "could not attach for JVMTI — Java hooks off, plain JNI still on");
            g_ctx.jvmti.store(nullptr);
            return;
        }

        jvmtiEnv* ti = nullptr;
        jint  rc    = 0;
        DWORD fault = 0;

        VLOG_INFO("jvm", "requesting jvmtiEnv (JVMTI 1.2)");
        if (!sehGetEnv(vm, reinterpret_cast<void**>(&ti), JVMTI_VERSION_1_2, &rc, &fault)) {
            char h[16]; hex8(fault, h);
            VLOG_ERR("jvm", "GetEnv(JVMTI) faulted (SEH 0x{}) — Java hooks off, plain JNI still on",
                     std::string(h));
            g_ctx.jvmti.store(nullptr);
            return;
        }

        if (rc != JNI_OK || !ti) {
            VLOG_ERR("jvm", "GetEnv(JVMTI) refused, rc={} — Java hooks off, plain JNI still on",
                     static_cast<int>(rc));
            g_ctx.jvmti.store(nullptr);
            return;
        }

        g_ctx.jvmti.store(ti);
        VLOG_INFO("jvm", "JavaVM* + jvmtiEnv acquired");
    });
}

void initFromRunningProcess() {
    HMODULE mod = vanta::platform::findJvmModule();
    if (!mod) {
        VLOG_FATAL("jvm", "target runtime not present in process");
        return;
    }
    auto jniProcName = wrath::strings::kJniGetCreatedJavaVMs.decrypt();
    auto p = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(GetProcAddress(mod, jniProcName.get()));
    if (!p) {
        VLOG_FATAL("jvm", "target runtime missing export");
        return;
    }

    VLOG_INFO("jvm", "target runtime found, initializing VM interface");

    JavaVM* vm    = nullptr;
    jsize   count = 0;

    for (int i = 0; i < 30; ++i) {                 // ~3s bounded retry
        jint  rc    = 0;
        DWORD fault = 0;

        if (!sehGetCreatedVMs(p, &vm, &count, &rc, &fault)) {
            char h[16]; hex8(fault, h);
            VLOG_ERR("jvm", "JNI_GetCreatedJavaVMs faulted (SEH 0x{}) — no Java side this session",
                     std::string(h));
            return;
        }

        if (rc == JNI_OK && count > 0 && vm) break;

        vm = nullptr;
        Sleep(100);
    }

    if (!vm) {
        VLOG_ERR("jvm", "No JavaVM created in the target process yet");
        return;
    }

    init(vm);
}

void requestCapabilities() {
    if (!g_ctx.jvmti.load()) return;

    jvmtiCapabilities caps;
    std::memset(&caps, 0, sizeof(caps));   // MSVC 14.29 ICEs on aggregate-init of this struct
    caps.can_generate_all_class_hook_events   = 1;
    caps.can_retransform_classes              = 1;
    caps.can_retransform_any_class            = 1;
    caps.can_redefine_classes                 = 1;
    caps.can_redefine_any_class               = 1;
    caps.can_generate_native_method_bind_events = 1;
    caps.can_get_bytecodes                    = 1;
    caps.can_get_constant_pool                = 1;
    caps.can_tag_objects                      = 1;
    caps.can_generate_vm_object_alloc_events  = 1;

    jvmtiError e = g_ctx.jvmti.load()->AddCapabilities(&caps);
    if (e != JVMTI_ERROR_NONE) {
        VLOG_ERR("jvm", "AddCapabilities failed: {} (bootstrap will still try)", static_cast<int>(e));
    }
}

JNIEnv* thisThreadEnv() {
    JavaVM* vm = g_ctx.vm.load(std::memory_order_relaxed);
    if (!vm) return nullptr;

    JNIEnv* env = nullptr;
    jint    r   = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (r == JNI_OK && env) {
        return env;
    }
    if (r == JNI_EDETACHED) {
        JavaVMAttachArgs a{JNI_VERSION_1_8, const_cast<char*>("Netty-Worker-0"), nullptr};
        if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &a) == JNI_OK) {
            return env;
        }
    }
    VLOG_ERR("jvm", "GetEnv/Attach failed for tid={}", static_cast<unsigned>(GetCurrentThreadId()));
    return nullptr;
}

void detachThisThreadIfAttached() {
    JavaVM* vm = g_ctx.vm.load(std::memory_order_relaxed);
    if (vm) {
        vm->DetachCurrentThread();
    }
}

JniScope::JniScope()  { env_ = thisThreadEnv(); }
JniScope::~JniScope() = default;

void shutdown() {
    if (g_ctx.jvmti.load()) {
        if (g_ctx.jvmti.load()->DisposeEnvironment() != JVMTI_ERROR_NONE) {
            vanta::blockUnload();
            return;
        }
        g_ctx.jvmti.store(nullptr);
    }
    g_ctx.vm.store(nullptr);
}

} // namespace vanta::jvm
