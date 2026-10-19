#include "SwordReplace.h"
#include "SwordReplaceResources.h"
#include "entry/Bootstrap.h"
#include "jvm/JvmContext.h"
#include <algorithm>

namespace vanta::modules::swordreplace {
namespace { const char* message = "Sword cosmetics - 20 models"; }
void update(bool enabled, int model) {
    if (!entry::bootstrapFinished() || !jvm::ctx().vm.load(std::memory_order_acquire)) {
        message = enabled ? "Waiting for Minecraft" : "Sword cosmetics - 20 models";
        return;
    }
    jvm::JniScope env;
    if (!env) { message = "Minecraft is not ready"; return; }
    const char* pending = prepareResources(env.get(), enabled,
        std::clamp(model, 0, static_cast<int>(std::size(models))-1));
    message = pending ? pending : (enabled ? "Cosmetic equipped" : "Sword cosmetics - 20 models");
}
const char* status() { return message; }
bool prepareShutdown() {
    jvm::JniScope env;
    return !env || restoreResources(env.get());
}
void shutdownCache() {
    jvm::JniScope env;
    if (env) shutdownResources(env.get());
}
}
