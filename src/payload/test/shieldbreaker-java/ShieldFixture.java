public final class ShieldFixture {
    public static final class Item {
        final String key;
        Item(String key) { this.key = key; }
        public String key() { return key; }
    }
    public static final class Stack {
        final Item item;
        Stack(String key) { item = new Item(key); }
        public boolean empty() { return item.key.isEmpty(); }
        public Item item() { return item; }
    }
    public static final class Inventory {
        public int selected;
        public boolean broken;
        final Stack[] slots = new Stack[9];
        Inventory() {
            for (int i = 0; i < 9; ++i) slots[i] = new Stack("");
            slots[0] = new Stack("item.minecraft.diamond_sword");
            slots[1] = new Stack("item.minecraft.netherite_pickaxe");
            slots[3] = new Stack("item.minecraft.iron_axe");
            slots[5] = new Stack("item.minecraft.netherite_axe");
            slots[8] = new Stack("item.minecraft.bread");
        }
        public Stack stack(int slot) {
            if (broken) throw new IllegalStateException("inventory unavailable");
            return slots[slot];
        }
        public void clearAxes() {
            slots[3] = new Stack("");
            slots[5] = new Stack("");
        }
    }
    public static final class Player {
        public Inventory inventory = new Inventory();
        public boolean blocking = true;
        public boolean using;
        public float health = 20;
        public float yaw;
        public double x;
        public double z;
        public double x() { return x; }
        public double z() { return z; }
        public boolean blocking() { return blocking; }
        public boolean using() { return using; }
        public float health() { return health; }
    }
    public static final class Hit {
        public Player target = new Player();
        public Player target() { return target; }
    }
    public Player player = new Player();
    public Object screen;
    public Hit hit = new Hit();
    public ShieldFixture() { player.z = 2; }
}
