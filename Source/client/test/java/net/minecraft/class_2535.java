package net.minecraft;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
public class class_2535 {
    public int sends = 0;
    public int receives = 0;
    public boolean flushed = false;
    public void method_52906(class_2596 packet, ChannelFutureListener listener, boolean flush) {
        if (packet.fail) throw new IllegalStateException("original send exception");
        sends++;
        flushed = flush;
    }
    protected void method_10770(ChannelHandlerContext context, class_2596 packet) {
        if (packet.fail) throw new IllegalStateException("original receive exception");
        receives++;
    }
}
