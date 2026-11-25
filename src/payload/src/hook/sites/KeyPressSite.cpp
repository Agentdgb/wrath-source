#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

static HookRow buildKeyPressSite() {
    return HookRow{
        HookId::KeyPress,
        "Keyboard",
        {
            {"onKey", "(JIIII)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        dt::asm_::OP_RETURN,
        "onKeyPress",
        "(IIII)I",
    };
}
DT_REGISTER_HOOK_SITE(buildKeyPressSite);

} // namespace dt::hook
