package net.minecraft;
public final class VisualFixture {}
enum class_1268 { field_5808, OFF_HAND }
enum class_1306 { field_6183, LEFT }
class class_1297 { boolean method_5805(){return true;} }
class class_1309 extends class_1297 {
 boolean field_6252=true,using=false;
 class_1306 arm=class_1306.field_6183;
 boolean method_6115(){return using;}
 class_1306 method_6068(){return arm;}
 int method_6028(){return 6;}
}
class class_742 extends class_1309 {}
class class_746 extends class_742 {}
class class_1657 extends class_1297 {}
class class_1569 extends class_1297 {}
class class_310 {
 static final class_310 INSTANCE=new class_310();
 final Thread thread=Thread.currentThread();
 class_746 field_1724=new class_746();
 class_1297 field_1692=null;
 static class_310 method_1551(){return INSTANCE;}
 boolean method_18854(){return Thread.currentThread()==thread;}
}
class class_4587 {
 int depth=0,translations=0,rotations=0;
 float lastX=0;
 void method_22903(){++depth;}
 void method_22909(){if(depth<=0)throw new IllegalStateException("matrix underflow");--depth;}
 void method_46416(float x,float y,float z){++translations;lastX=x;}
 void method_22907(org.joml.Quaternionfc q){++rotations;}
}
class class_1799 {}
class class_11659 {}
class class_759 {
 float lastSwing=-1;
 int depthSeen=0,calls=0;
 boolean fail=false;
 void method_3228(class_742 player,float delta,float pitch,class_1268 hand,float swing,
                  class_1799 item,float equip,class_4587 matrices,class_11659 queue,int light){
  lastSwing=swing;depthSeen=matrices.depth;++calls;
  if(fail)throw new IllegalStateException("original render failure");
 }
}
class class_758 {void method_71108(){}}
class class_2663 {
 byte field_12174;
 class_2663(byte value){field_12174=value;}
 byte method_11470(){return field_12174;}
}
