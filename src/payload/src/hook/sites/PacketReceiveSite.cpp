#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace vanta::hook {

static HookRow buildPacketReceiveSite() {
    return HookRow{
        HookId::PacketReceive,
        "ClientConnection",
        {
            {"channelRead0", "(Lio/netty/channel/ChannelHandlerContext;LPacket;)V"},
            {"handlePacket", "(LPacket;LPacketListener;)V"},
        },
        Recipe::A_PreCancel,
        HookLoad::Installed,
        vanta::asm_::OP_RETURN,
        "onPacketReceive",
        "(Ljava/lang/Object;)I",
    };
}
VANTA_REGISTER_HOOK_SITE(buildPacketReceiveSite);

} // namespace vanta::hook
