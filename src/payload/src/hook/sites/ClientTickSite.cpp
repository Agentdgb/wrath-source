#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

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
        dt::asm_::OP_RETURN,
        "onClientTick",
        "()I",
    };
}

DT_REGISTER_HOOK_SITE(buildClientTickSite);

} // namespace dt::hook
