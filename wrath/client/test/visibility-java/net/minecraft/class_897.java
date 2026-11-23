package net.minecraft;
import net.caffeinemc.mods.sodium.client.render.SodiumWorldRenderer;
public class class_897 {
    public boolean method_3933(class_1297 entity,class_4604 frustum,double x,double y,double z) {
        return wrapOperation$sodium$preShouldRender(entity);
    }
    private boolean wrapOperation$sodium$preShouldRender(class_1297 entity) {
        // Matches the typed external call in the user's Sodium crash report.
        return SodiumWorldRenderer.isEntityVisible(this,entity);
    }
}
