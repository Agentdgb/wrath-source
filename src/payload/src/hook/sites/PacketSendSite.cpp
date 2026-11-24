#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

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
        vanta::asm_::OP_RETURN,
        "onPacketSend",
        "(Ljava/lang/Object;)I",
    };
}
VANTA_REGISTER_HOOK_SITE(buildPacketSendSite);

} // namespace vanta::hook
