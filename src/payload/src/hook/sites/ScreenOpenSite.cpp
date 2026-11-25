#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

static HookRow buildScreenOpenSite() {
    return HookRow{
        HookId::ScreenOpen,
        "MinecraftClient",
        {
            {"setScreen",     "(LScreen;)V"},
            {"openScreen",    "(LScreen;)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        dt::asm_::OP_RETURN,
        "onScreenOpen",
        "(Ljava/lang/Object;)I",
    };
}
DT_REGISTER_HOOK_SITE(buildScreenOpenSite);

} // namespace dt::hook
