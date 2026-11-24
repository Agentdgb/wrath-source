package net.minecraft;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import java.util.Queue;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.function.Consumer;
public class class_2535 {
    public int sends = 0;
    public int receives = 0;
    public boolean flushed = false;
    public boolean connected = true;
    private final Queue<Consumer<class_2535>> pending = new ConcurrentLinkedQueue<>();
    // Share the (ClientConnection)V descriptor with the Consumer bootstrap,
    // as in the game. Renaming that descriptor makes a queued lambda cast its
    // argument to the copy, reproducing the server-list ClassCastException.
    public class_2535() { checkConnection(this); }
    private static void checkConnection(class_2535 connection) {}
    public void method_52906(class_2596 packet, ChannelFutureListener listener, boolean flush) {
        if (connected) {
            method_10751();
            method_36942(packet, listener, flush);
        } else {
            // Match ClientConnection's delayed send: the lambda must accept a
            // real connection when the server-list tick drains this queue.
            pending.add(connection -> connection.method_36942(packet, listener, flush));
        }
    }
    public void method_10751() {
        Consumer<class_2535> action;
        while ((action = pending.poll()) != null) action.accept(this);
    }
    private void method_36942(class_2596 packet, ChannelFutureListener listener, boolean flush) {
        if (packet.fail) throw new IllegalStateException("original send exception");
        sends++;
        flushed = flush;
    }
    protected void method_10770(ChannelHandlerContext context, class_2596 packet) {
        if (packet.fail) throw new IllegalStateException("original receive exception");
        receives++;
    }
}
