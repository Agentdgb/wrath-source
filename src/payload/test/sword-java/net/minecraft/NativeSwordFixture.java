package net.minecraft;
import java.util.*;
import java.util.function.*;
import org.joml.*;

// No pack-manager or resource-reload API exists in this fixture.
public final class NativeSwordFixture {
    static class_10444 state=new class_10444();
    static class_11661 queue=new class_11661();
    static class_4588 consumer=new class_4588();
    static class_10439 lookup(String name) {
        String[] id=name.split(":",2);
        return class_310.instance.field_55226.field_55334.apply(class_2960.method_60655(id[0],id[1]));
    }
    public static int render(String name,int mode,boolean flush) {
        state=new class_10444();
        lookup(name).method_65584(state,new class_1799(),class_310.instance.field_55226,
            class_811.values()[mode],null,null,42);
        consumer=new class_4588();
        if(state.layer!=null) {
            if(state.layer.transform==null || state.layer.bounds.get().length!=8 || !state.light || !state.animated)
                throw new AssertionError("Incomplete native render state");
            state.layer.special.method_72175(point -> {if(point==null)throw new AssertionError();});
            state.layer.special.method_65694(state.layer.data,class_811.values()[mode],new class_4587(),queue,0xF000F0,0xA0000,false,0);
        }
        if(flush)queue.flush();
        return consumer.vertices;
    }
    public static int queued() {return queue.draws.size();}
    public static int textures() {return class_310.instance.manager.textures.size();}
    public static boolean restored() {return state.layer==null && queue.draws.isEmpty() && class_310.instance.field_55226.field_55334==class_10442.original;}
    public static boolean untouched(String name) {return lookup(name)==class_10442.vanilla;}
    public static void replaceManager() {class_310.instance.field_55226=new class_10442();}
}
class class_310 {
    static final class_310 instance=new class_310();
    public class_10442 field_55226=new class_10442();
    final class_1060 manager=new class_1060();
    public static class_310 method_1551() {return instance;}
    public class_1060 method_1531() {return manager;}
}
interface class_10439 {
    void method_65584(class_10444 state,class_1799 stack,class_10442 manager,class_811 context,class_638 world,class_11566 holder,int seed);
}
class class_10442 {
    static final class_10439 vanilla=(s,i,m,c,w,h,seed)->{};
    static final Function<class_2960,class_10439> original=id->vanilla;
    public Function<class_2960,class_10439> field_55334=original;
}
class class_10444 {
    public class_811 field_55337;
    class_10446 layer;
    boolean animated,light,oversized;
    public void method_65605() {layer=null;animated=false;light=false;oversized=false;}
    public class_10446 method_65601() {layer=new class_10446();return layer;}
    public void method_70947() {animated=true;}
    public void method_72172(boolean value) {oversized=value;}
    class class_10446 {
        class_10515 special;Object data;class_804 transform;Supplier<Vector3f[]> bounds;
        public void method_65617(class_10515 value,Object arg) {special=value;data=arg;}
        public void method_67993(class_804 value) {transform=value;}
        public void method_67995(Supplier<Vector3f[]> value) {bounds=value;}
        public void method_67996(boolean value) {light=value;}
    }
}
interface class_10515 {
    void method_65694(Object data,class_811 context,class_4587 stack,class_11659 queue,int light,int overlay,boolean glint,int outline);
    void method_72175(Consumer<Vector3f> consumer);
    Object method_65695(class_1799 stack);
}
enum class_811 {NONE,THIRD_PERSON_LEFT_HAND,THIRD_PERSON_RIGHT_HAND,FIRST_PERSON_LEFT_HAND,FIRST_PERSON_RIGHT_HAND,HEAD,GUI,GROUND,FIXED,ON_SHELF}
class class_804 {
    final Vector3fc rotation,translation,scale;
    public class_804(Vector3fc rotation,Vector3fc translation,Vector3fc scale) {this.rotation=rotation;this.translation=translation;this.scale=scale;}
}
class class_1799 {}
class class_638 {}
class class_11566 {}
class class_4587 {
    static class class_4665 {
        public Matrix4f method_23761() {return new Matrix4f();}
        public Matrix3f method_23762() {return new Matrix3f();}
    }
}
class class_4588 {
    int vertices;
    public void method_23919(float x,float y,float z,int color,float u,float v,int overlay,int light,float nx,float ny,float nz) {
        if(!Float.isFinite(x+y+z+u+v+nx+ny+nz) || color!=-1 || overlay!=0xA0000 || light!=0xF000F0 || Math.abs(nx*nx+ny*ny+nz*nz-1)>0.001)
            throw new AssertionError("Invalid native vertex");
        ++vertices;
    }
}
interface class_11785 {void method_73483(class_4587 matrices,class_1921 layer,class_11659.class_11660 draw);}
interface class_11659 extends class_11785 {
    interface class_11660 {void render(class_4587.class_4665 pose,class_4588 vertices);}
}
class class_11661 implements class_11659 {
    final List<class_11659.class_11660> draws=new ArrayList<>();
    public void method_73483(class_4587 matrices,class_1921 layer,class_11659.class_11660 draw) {draws.add(draw);}
    public void method_72953() {draws.clear();}
    void flush() {for(var draw:draws)draw.render(new class_4587.class_4665(),NativeSwordFixture.consumer);draws.clear();}
}
record class_2960(String namespace,String path) {
    public static class_2960 method_60655(String namespace,String path) {return new class_2960(namespace,path);}
    public String method_12836() {return namespace;}
    public String method_12832() {return path;}
}
class class_1011 {
    public static class_1011 method_49277(byte[] png) {
        if(png.length<24 || png[0]!=(byte)137 || png[1]!=80 || png[2]!=78 || png[3]!=71)throw new AssertionError("Invalid embedded PNG");
        return new class_1011();
    }
}
class class_1044 {}
class class_1043 extends class_1044 {
    public class_1043(Supplier<String> name,class_1011 image) {if(!name.get().contains("native sword"))throw new AssertionError();}
}
class class_1060 {
    final Map<class_2960,class_1044> textures=new HashMap<>();
    public void method_4616(class_2960 id,class_1044 texture) {if(textures.put(id,texture)!=null)throw new AssertionError("Duplicate texture");}
    public void method_4615(class_2960 id) {if(textures.remove(id)==null)throw new AssertionError("Destroying unowned texture");}
}
class class_1921 {}
class class_12249 {
    public static class_1921 method_75982(class_2960 id) {if(!class_310.instance.manager.textures.containsKey(id))throw new AssertionError();return new class_1921();}
}
