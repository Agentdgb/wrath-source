public final class InputFixture {
    public static final class Key {
        public int clickCount;
        boolean consume() {
            if (clickCount <= 0) return false;
            --clickCount;
            return true;
        }
    }
    public final Key use = new Key();
    public int selected;
    public int wireSlot;
    public int count = 2;
    public String packets = "";

    // The part of vanilla input relevant to this transport: slot synchronization,
    // consumed use-key presses, then the player's movement send. No callbacks.
    public void tick() {
        if (selected != wireSlot) {
            packets += "slot:" + selected + ";";
            wireSlot = selected;
        }
        while (use.consume()) {
            if (count > 0) {
                --count;
                packets += "use;";
            }
        }
        packets += "move;";
    }
}
