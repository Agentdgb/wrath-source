#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

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
        vanta::asm_::OP_RETURN,
        "onGameLoop",
        "(F)I",
    };
}
VANTA_REGISTER_HOOK_SITE(buildGameLoopSite);

} // namespace vanta::hook
