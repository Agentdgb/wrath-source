#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

static HookRow buildAttackSite() {
    return HookRow{
        HookId::Attack,
        "ClientPlayerInteractionManager",
        {
            {"attackEntity", "(LPlayerEntity;LEntity;)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        vanta::asm_::OP_RETURN,
        "onAttackEntity",
        "(Ljava/lang/Object;)I",
    };
}
VANTA_REGISTER_HOOK_SITE(buildAttackSite);

} // namespace vanta::hook
