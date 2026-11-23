package net.caffeinemc.mods.sodium.client.render;
import net.minecraft.class_1297;
import net.minecraft.class_897;
public final class SodiumWorldRenderer {
    public static int calls;
    public static boolean isEntityVisible(class_897 renderer,class_1297 entity) {
        if(renderer==null)throw new AssertionError("Wrong renderer identity");
        ++calls;
        return entity.visible;
    }
}
