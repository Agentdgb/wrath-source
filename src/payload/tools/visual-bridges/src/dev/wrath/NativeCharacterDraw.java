package dev.wrath;
import net.minecraft.*;
public final class NativeCharacterDraw implements class_11659.class_11660 {
 public final long key; public final int light,overlay; public final float pitch,yaw,roll,bent,turn;
 public NativeCharacterDraw(long key,int light,int overlay,float pitch,float yaw,float roll,float bent,float turn){this.key=key;this.light=light;this.overlay=overlay;this.pitch=pitch;this.yaw=yaw;this.roll=roll;this.bent=bent;this.turn=turn;}
 public native void render(class_4587.class_4665 p,class_4588 v);
}