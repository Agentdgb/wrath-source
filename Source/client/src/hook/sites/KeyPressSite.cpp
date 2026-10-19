#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

static HookRow buildKeyPressSite() {
    return HookRow{
        HookId::KeyPress,
        "Keyboard",
        {
            {"onKey", "(JIIII)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        vanta::asm_::OP_RETURN,
        "onKeyPress",
        "(IIII)I",
    };
}
VANTA_REGISTER_HOOK_SITE(buildKeyPressSite);

} // namespace vanta::hook
