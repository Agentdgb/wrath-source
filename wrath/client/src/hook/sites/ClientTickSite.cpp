#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

static HookRow buildClientTickSite() {
    return HookRow{
        HookId::ClientTick,
        "MinecraftClient",
        {
            {"tick",      "()V"},
            {"clientTick","()V"},
            // Vanilla obf drift is absorbed by MappingRegistry lookup — the
            // *canon* name is what we probe against.
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        vanta::asm_::OP_RETURN,
        "onClientTick",
        "()I",
    };
}

VANTA_REGISTER_HOOK_SITE(buildClientTickSite);

} // namespace vanta::hook
