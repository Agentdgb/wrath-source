package net.minecraft;
import java.util.ArrayList;

/** Small real-JVM world for the native adapter, not a replacement for the Java port oracle. */
public final class AimFixture {
    static class_1297 rival;
    static boolean failTurn, blocked, halfBlock;
    static int turns, rays;
    public static void reset() {
        class_310.instance = new class_310();
        var client = class_310.instance;
        class_310.field_1700 = client;
        client.field_1687 = new class_638(); client.field_1724 = new class_746();
        client.field_1724.world = client.field_1687; client.field_1724.field_6031 = -40;
        rival = new class_1657(); rival.x = 3; rival.world = client.field_1687;
        client.field_1687.entities.add(client.field_1724); client.field_1687.entities.add(rival);
        turns = rays = 0; failTurn = blocked = halfBlock = false;
    }
    public static void set(int field, double value) {
        var client = class_310.instance;
        switch(field) {
            case 1 -> rival.x = value;
            case 2 -> client.field_1724.field_6031 = (float)value;
            case 3 -> client.field_1724.field_5965 = (float)value;
            case 4 -> rival.alive = value > 0;
            case 5 -> {var other = new class_1657();other.x = 3;other.z = 2;other.world=client.field_1687;client.field_1687.entities.add(other);}
            case 6 -> client.field_1755 = value != 0 ? new class_437() : null;
            case 7 -> client.field_1724.held = value == 0 ? "air" : value == 1 ? "sword" : value == 2 ? "axe" : value == 3 ? "mace" : value == 4 ? "trident" : "tagged_custom_blade";
            case 9 -> {client.field_1687 = new class_638();client.field_1724.world=client.field_1687;}
            case 10 -> rival.height = value;
            case 11 -> rival.invisible = value != 0;
            case 12 -> rival.spectator = value != 0;
            case 13 -> client.field_1687 = null;
            case 14 -> client.field_1724.alive = value > 0;
            case 15 -> {client.field_1687.entities.clear();rival.alive=false;}
            case 17 -> failTurn = value != 0;
            case 18 -> rival.z = value;
            case 19 -> {
                client.field_1687.entities.remove(rival);
                rival = value == 1 ? new class_1308() : value == 2 ? new class_1429() : value == 3 ? new class_1646() : value == 4 ? new class_1511() : value == 5 ? new class_1531() : new class_1657();
                rival.x=3;rival.world=client.field_1687;client.field_1687.entities.add(rival);
            }
            case 20 -> blocked = value != 0;
            case 21 -> halfBlock = value != 0;
            case 22 -> client.paused = value != 0;
            case 23 -> rival.velocityX = value;
            case 24 -> rival.velocityZ = value;
            case 25 -> client.field_1690.field_1886.pressed = value != 0;
            case 26 -> client.field_1690.sensitivity = value;
            case 27 -> client.field_1765 = value == 0 ? null : new class_3965(class_239.class_240.BLOCK);
            case 28 -> client.field_1724.field_6273 = (int)value;
            case 29 -> client.field_1724.usingItem = value != 0;
        }
    }
    public static double get(int field) {return field==1 ? class_310.instance.field_1724.field_6031 : field==2 ? class_310.instance.field_1724.field_5965 : field==4 ? rays : turns;}
    public static Object target() {return rival;}
    public static Object otherTarget() {var entities = class_310.instance.field_1687.entities; return entities.get(entities.size() - 1);}
}
class class_243 {
    public double field_1352,field_1351,field_1350;
    public class_243(double x,double y,double z){field_1352=x;field_1351=y;field_1350=z;}
}
class class_238 {
    public double field_1323,field_1322,field_1321,field_1320,field_1325,field_1324;
    class_238(double x,double y,double z,double height){field_1323=x-.3;field_1322=y;field_1321=z-.3;field_1320=x+.3;field_1325=y+height;field_1324=z+.3;}
}
class class_1297 {
    double x,y,z,height=1.8,velocityX,velocityZ;
    public float field_6031,field_5965,field_5982,field_6004;boolean invisible,spectator,alive=true;
    class_1937 world;
    public double method_23317(){return x;} public double method_23318(){return y;} public double method_23321(){return z;}
    public float method_36454(){return field_6031;}public float method_36455(){return field_5965;}
    public void method_36456(float value){if(AimFixture.failTurn)throw new IllegalStateException("rotation failure");field_6031=value;AimFixture.turns++;}
    public void method_36457(float value){if(AimFixture.failTurn)throw new IllegalStateException("rotation failure");field_5965=value;AimFixture.turns++;}
    public float method_5751(){return 1.62f;} public boolean method_24828(){return true;}
    public boolean method_5805(){return alive;}public boolean method_7325(){return spectator;}public boolean method_5767(){return invisible;}
    public class_238 method_5829(){return new class_238(x,y,z,height);}
    public class_243 method_33571(){return new class_243(x,y+1.62,z);}
    public class_243 method_18798(){return new class_243(velocityX,0,velocityZ);}
}
class class_1309 extends class_1297 {
    String held="sword";
    boolean usingItem = false;
    public class_1799 method_6047(){return new class_1799(held);}
    public boolean method_6057(class_1297 entity){return !AimFixture.blocked&&!AimFixture.halfBlock;}
    public boolean method_6115(){return usingItem;}
}
class class_1657 extends class_1309 { public int field_6273; } class class_746 extends class_1657 {}
class class_1308 extends class_1309 {} class class_1429 extends class_1308 {} class class_1646 extends class_1308 {}
class class_1511 extends class_1297 {} class class_1531 extends class_1309 {}
class class_6862 {final String kind;class_6862(String kind){this.kind=kind;}}
class class_3489 {public static final class_6862 field_42611=new class_6862("sword"),field_42612=new class_6862("axe");}
class class_1792 {final String key;class_1792(String key){this.key=key;}}
class class_1802 {public static final class_1792 field_49814=new class_1792("mace"),field_8547=new class_1792("trident");}
class class_1799 {
    final String key;class_1799(String key){this.key=key;}
    public boolean method_31573(class_6862 tag){return key.equals(tag.kind)||(key.equals("tagged_custom_blade")&&tag.kind.equals("sword"));}
    public boolean method_31574(class_1792 item){return key.equals(item.key);}
}
interface class_1922 {class_3965 method_17742(class_3959 context);}
class class_1937 implements class_1922 {
    public class_3965 method_17742(class_3959 context){AimFixture.rays++;return new class_3965(AimFixture.blocked||(AimFixture.halfBlock&&context.end.field_1351<1.0)?class_239.class_240.BLOCK:class_239.class_240.field_1333);}
}
class class_638 extends class_1937 {final ArrayList<class_1297> entities=new ArrayList<>();public Iterable<class_1297> method_18112(){return entities;}}
class class_304 {boolean pressed=true;public boolean method_1434(){return pressed;}}
class class_315 {public class_304 field_1886=new class_304(); double sensitivity=.5; public class_7172 method_42495(){return new class_7172(sensitivity);}}
class class_7172 {final double value;class_7172(double value){this.value=value;}public Object method_41753(){return Double.valueOf(value);}}
class class_437 {}
class class_239 {
    public enum class_240 {field_1333,BLOCK}
    final class_240 type;class_239(class_240 type){this.type=type;}
    public class_240 method_17783(){return type;}
}
class class_3965 extends class_239 {class_3965(class_240 type){super(type);}}
class class_3959 {
    public enum class_3960 {field_17558}
    public enum class_242 {field_1348}
    final class_243 end;
    public class_3959(class_243 start,class_243 end,class_3960 shape,class_242 fluid,class_1297 entity){this.end=end;}
}
class class_310 {
    static class_310 instance; public static class_310 field_1700;
    public class_746 field_1724;public class_638 field_1687;public class_437 field_1755;public class_239 field_1765;
    public class_315 field_1690=new class_315();boolean paused;
    public static class_310 method_1551(){return instance;}
    public boolean method_18854(){return true;}
    public boolean method_1493(){return paused;}
}
