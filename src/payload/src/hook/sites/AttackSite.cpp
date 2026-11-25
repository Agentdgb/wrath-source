#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

static HookRow buildAttackSite() {
    return HookRow{
        HookId::Attack,
        "ClientPlayerInteractionManager",
        {
            {"attackEntity", "(LPlayerEntity;LEntity;)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        dt::asm_::OP_RETURN,
        "onAttackEntity",
        "(Ljava/lang/Object;)I",
    };
}
DT_REGISTER_HOOK_SITE(buildAttackSite);

} // namespace dt::hook
