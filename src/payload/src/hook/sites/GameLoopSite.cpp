#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

static HookRow buildGameLoopSite() {
    return HookRow{
        HookId::GameLoop,
        "MinecraftClient",
        {
            {"runGameLoop",   "(Z)V"},
            {"render",        "(FJZ)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        dt::asm_::OP_RETURN,
        "onGameLoop",
        "(F)I",
    };
}
DT_REGISTER_HOOK_SITE(buildGameLoopSite);

} // namespace dt::hook
