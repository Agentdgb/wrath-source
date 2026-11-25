#include "asm/Opcodes.h"
#include "hook/HookRow.h"

namespace dt::hook {

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
        dt::asm_::OP_RETURN,
        "onPacketReceive",
        "(Ljava/lang/Object;)I",
    };
}
DT_REGISTER_HOOK_SITE(buildPacketReceiveSite);

} // namespace dt::hook
