#include "hook/HookManager.h"

#include "asm/ClassReader.h"
#include "asm/ClassWriter.h"
#include "asm/CodeEditor.h"
#include "asm/Opcodes.h"
#include "asm/StackMapRebuilder.h"
#include "hook/HookRow.h"
#include "hook/bridge/HookBridge.h"
#include "hook/bridge/HookBridgeClass.h"
#include "jvm/JvmContext.h"
#include "mappings/MappingRegistry.h"
#include "util/Log.h"

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace vanta::hook {

// ── ClassFileLoadHook trampoline ─────────────────────────────────────────────
namespace {

void JNICALL classFileLoadHookThunk(
    jvmtiEnv* jvmti, JNIEnv* /*env*/, jclass /*classBeingRedefined*/, jobject /*loader*/,
    const char* name, jobject /*protectionDomain*/,
    jint classDataLen, const unsigned char* classData,
    jint* newClassDataLen, unsigned char** newClassData)
{
    if (!name || !classData || classDataLen <= 0) return;

    std::uint8_t* rewritten = nullptr;
    std::size_t   rewrittenLen = 0;
    if (!HookManager::get().transformIncoming(
            std::string_view(name),
            reinterpret_cast<const std::uint8_t*>(classData),
            static_cast<std::size_t>(classDataLen),
            &rewritten, &rewrittenLen)) {
        return;
    }

    // JVMTI takes ownership; must allocate via Allocate so JVMTI Deallocate can free it.
    unsigned char* jvmtiBuf = nullptr;
    if (jvmti->Allocate(static_cast<jlong>(rewrittenLen), &jvmtiBuf) != JVMTI_ERROR_NONE) {
        delete[] rewritten;
        return;
    }
    std::memcpy(jvmtiBuf, rewritten, rewrittenLen);
    delete[] rewritten;
    *newClassDataLen = static_cast<jint>(rewrittenLen);
    *newClassData    = jvmtiBuf;
}

void JNICALL vmDeathThunk(jvmtiEnv*, JNIEnv*) {
    VLOG_INFO("hook", "VMDeath — bus is closing");
}

} // namespace

struct HookManager::Impl {
    std::mutex                                                   mx;
    ankerl::unordered_dense::map<std::string, std::vector<std::uint8_t>> originalBytes;
    std::atomic<std::size_t>                                     transformed{0};
    std::atomic<bool>                                            armed[static_cast<std::size_t>(HookId::_Count)]{};
    jclass                                                       bridgeClass = nullptr;   // global
    bool                                                         installed = false;
};

HookManager& HookManager::get() {
    static HookManager g;
    return g;
}

HookManager::HookManager()  : impl_(new Impl()) {
    // Installed hooks are armed by default; lazy hooks start disarmed.
    for (auto& r : HookRegistry::get().rows()) {
        impl_->armed[static_cast<std::size_t>(r.id)].store(r.load == HookLoad::Installed);
    }
}
HookManager::~HookManager() { delete impl_; }

void HookManager::install(jvmtiEnv* jvmti, JNIEnv* env) {
    if (!jvmti || !env) { VLOG_ERR("hook", "install() with null jvmti/env"); return; }
    if (impl_->installed) return;

    // 1) Define HookBridge into the bootstrap loader (null class loader).
    //
    // Optional path. Every transform this manager can apply rewrites a method
    // to call `<bridge>::onXxx(...)`, so with no shim class there
    // is nothing valid to inject and we must NOT arm the retransform — doing
    // so would raise NoClassDefFoundError inside Minecraft. Bailing out here
    // leaves the client fully functional: JNIHook-based modules (triggerbot,
    // autototem) install their own detours and never touch this manager.
    impl_->bridgeClass = bridge::defineHookBridgeClass(env);
    if (!impl_->bridgeClass) {
        VLOG_INFO("hook", "HookBridge shim absent — bytecode-transform hooks stay dormant "
                          "(JNIHook modules unaffected).");
        return;
    }

    // 2) Register natives on it.
    bridge::registerNatives(env, impl_->bridgeClass);

    // 3) Wire the JVMTI callback.
    jvmtiEventCallbacks cbs{};
    cbs.ClassFileLoadHook = &classFileLoadHookThunk;
    cbs.VMDeath           = &vmDeathThunk;
    if (jvmti->SetEventCallbacks(&cbs, sizeof(cbs)) != JVMTI_ERROR_NONE) {
        VLOG_ERR("hook", "SetEventCallbacks failed");
        return;
    }
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, nullptr);

    impl_->installed = true;
    VLOG_INFO("hook", "HookManager installed ({} sites registered)", HookRegistry::get().rows().size());

    retransformKnown();
}

void HookManager::retransformKnown() {
    auto& ctx = vanta::jvm::ctx();
    if (!ctx.jvmti.load()) return;

    jint    n     = 0;
    jclass* all   = nullptr;
    if (ctx.jvmti.load()->GetLoadedClasses(&n, &all) != JVMTI_ERROR_NONE) return;

    std::vector<jclass> targets;
    targets.reserve(64);

    auto env = vanta::jvm::thisThreadEnv();
    for (jint i = 0; i < n; ++i) {
        char* sig = nullptr;
        if (ctx.jvmti.load()->GetClassSignature(all[i], &sig, nullptr) != JVMTI_ERROR_NONE || !sig) continue;
        // sig is "Lnet/minecraft/class_310;" — strip 'L' and ';'.
        std::string_view sv(sig);
        if (sv.size() >= 3 && sv.front() == 'L' && sv.back() == ';') sv = sv.substr(1, sv.size() - 2);

        auto canon = vanta::mappings::MappingRegistry::get().canonForRuntimeClass(sv);
        if (!canon.empty()) targets.push_back(all[i]);
        ctx.jvmti.load()->Deallocate(reinterpret_cast<unsigned char*>(sig));
    }
    (void)env;

    if (targets.empty()) {
        VLOG_INFO("hook", "No loaded classes match canonical targets (yet)");
        ctx.jvmti.load()->Deallocate(reinterpret_cast<unsigned char*>(all));
        return;
    }

    constexpr jint kBatch = 32;
    std::size_t installed = 0;
    for (std::size_t i = 0; i < targets.size(); i += kBatch) {
        jint chunk = static_cast<jint>(std::min<std::size_t>(kBatch, targets.size() - i));
        jvmtiError r = ctx.jvmti.load()->RetransformClasses(chunk, &targets[i]);
        if (r == JVMTI_ERROR_UNMODIFIABLE_CLASS) {
            VLOG_WARN("hook", "Batch contained unmodifiable classes; retrying one by one");
            for (jint k = 0; k < chunk; ++k) {
                if (ctx.jvmti.load()->RetransformClasses(1, &targets[i + k]) == JVMTI_ERROR_NONE) ++installed;
            }
        } else if (r != JVMTI_ERROR_NONE) {
            VLOG_WARN("hook", "RetransformClasses batch failed: {}", static_cast<int>(r));
        } else {
            installed += chunk;
        }
    }
    ctx.jvmti.load()->Deallocate(reinterpret_cast<unsigned char*>(all));
    VLOG_INFO("hook", "Retransformed {} classes at bootstrap", installed);
}

void HookManager::armHook(HookId h) {
    impl_->armed[static_cast<std::size_t>(h)].store(true);
    retransformKnown();
}
void HookManager::disarmHook(HookId h) {
    impl_->armed[static_cast<std::size_t>(h)].store(false);
    retransformKnown();
}

bool HookManager::transformIncoming(std::string_view runtimeClassName,
                                    const std::uint8_t* classBytes,
                                    std::size_t         classLen,
                                    std::uint8_t**      newBytes,
                                    std::size_t*        newLen)
{
    auto canon = vanta::mappings::MappingRegistry::get().canonForRuntimeClass(runtimeClassName);
    if (canon.empty()) return false;

    // Find rows targeting this owner.
    std::vector<const HookRow*> owned;
    for (auto& r : HookRegistry::get().rows()) {
        if (r.ownerCanon != canon) continue;
        if (!impl_->armed[static_cast<std::size_t>(r.id)].load()) continue;
        owned.push_back(&r);
    }
    if (owned.empty()) return false;

    // Parse the class.
    vanta::asm_::ClassView v;
    if (!vanta::asm_::ClassReader::parse(classBytes, classLen, v)) {
        VLOG_WARN("hook", "ClassReader::parse failed for '{}'", runtimeClassName);
        return false;
    }

    // For v0.1 we apply *one* row per pass; multi-row per class requires
    // chaining rewrites through the same CP extender, which is a follow-up.
    const HookRow* row = owned.front();

    // Pick first candidate that resolves.
    const asm_::MethodView* mv     = nullptr;
    std::string             mName, mDesc;
    for (auto& cand : row->candidates) {
        auto mr = vanta::mappings::MappingRegistry::get().methodFor(row->ownerCanon, cand.canonName);
        if (mr.name.empty()) continue;
        mName.assign(mr.name); mDesc.assign(mr.desc);
        for (auto& m : v.methods) {
            if (v.utf8At(m.nameIndex) == mName && v.utf8At(m.descIndex) == mDesc) {
                mv = &m; break;
            }
        }
        if (mv) break;
    }
    if (!mv) {
        VLOG_DEBUG("hook", "No candidate resolved for {}.{}", canon, toString(row->id));
        return false;
    }

    asm_::RewriteOneMethod r(v);
    r.targetMethod = static_cast<std::uint16_t>(mv - v.methods.data());

    switch (row->recipe) {
    case Recipe::A_PreCancel: {
        if (!asm_::emitRecipeA_PreCancel(v, *mv, bridge::kBridgeClassInternalName,
                                         row->bridgeMethod, row->bridgeDesc, row->returnOpcode, r)) {
            return false;
        }
        break;
    }
    case Recipe::B_ReturnRewrite:
        // Recipe B — see hook/recipes/RecipeB_ReturnRewrite.cpp; not wired in v0.1.
        VLOG_DEBUG("hook", "Recipe B not implemented for site {}", toString(row->id));
        return false;
    }

    auto out = vanta::asm_::ClassWriter::emit(v, r);
    if (out.empty()) return false;

    // Cache originals for later restoration.
    {
        std::scoped_lock lk(impl_->mx);
        impl_->originalBytes[std::string(runtimeClassName)].assign(classBytes, classBytes + classLen);
    }

    auto* copy = new std::uint8_t[out.size()];
    std::memcpy(copy, out.data(), out.size());
    *newBytes = copy;
    *newLen   = out.size();
    impl_->transformed.fetch_add(1, std::memory_order_relaxed);
    VLOG_INFO("hook", "Patched {} ({} bytes -> {} bytes) with site {}",
              runtimeClassName, classLen, out.size(), toString(row->id));
    return true;
}

void HookManager::uninstall() {
    auto& ctx = vanta::jvm::ctx();
    if (!ctx.jvmti.load()) return;
    ctx.jvmti.load()->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
    ctx.jvmti.load()->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_VM_DEATH, nullptr);
    impl_->installed = false;
}

std::size_t HookManager::transformedClassCount() const noexcept {
    return impl_->transformed.load(std::memory_order_relaxed);
}

} // namespace vanta::hook
