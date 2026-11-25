#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

static HookRow buildPacketSendSite() {
    return HookRow{
        HookId::PacketSend,
        "ClientConnection",
        {
            {"sendPacket",       "(LPacket;)V"},
            {"send",             "(LPacket;)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        dt::asm_::OP_RETURN,
        "onPacketSend",
        "(Ljava/lang/Object;)I",
    };
}
DT_REGISTER_HOOK_SITE(buildPacketSendSite);

} // namespace dt::hook
