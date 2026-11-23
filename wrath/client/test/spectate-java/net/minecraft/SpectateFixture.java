package net.minecraft;
import java.util.*;
import com.mojang.authlib.GameProfile;
public final class SpectateFixture {
 public static int cameraId(){return class_310.method_1551().field_1719.id;}
 public static void removeBeta(){class_310.method_1551().field_1687.players.removeIf(p->p.id==3);}
 public static void addBeta(){class_310.method_1551().field_1687.players.add(new class_1657(3,"Beta"));}
 public static void disconnect(){class_310.method_1551().field_1687=null;}
 public static void allowServer(){class_634.allow=true;class_634.commands=0;}
 public static void denyServer(){class_634.allow=false;class_634.commands=0;class_310.method_1551().field_1724.spectator=false;}
 public static int commandCount(){return class_634.commands;}
 public static boolean spectator(){return class_310.method_1551().field_1724.spectator;}
 public static void releaseCamera(){class_310.method_1551().field_1719=class_310.method_1551().field_1724;}
}
class class_1297 {
 int id; float yaw=10,pitch=5;boolean spectator=false;
 class_1297(int id){this.id=id;}
 int method_5628(){return id;}
 float method_36454(){return yaw;}
 float method_36455(){return pitch;}
 boolean method_7325(){return spectator;}
}
class class_1309 extends class_1297 {int field_6235=0;class_1309(int id){super(id);}}
class class_1657 extends class_1309 {
 GameProfile profile;String account;
 class_1657(int id,String name){super(id);account=name;profile=new GameProfile(new UUID(0,id),name);}
 GameProfile method_7334(){return profile;}
}
class class_746 extends class_1657 {
 class_746(){super(1,"Local");}
 boolean method_3134(){return class_310.method_1551().field_1719==this;}
}
class class_638 {
 List<class_1657> players=new ArrayList<>();
 List<class_1657> method_18456(){return players;}
 class_1297 method_8469(int id){return players.stream().filter(p->p.id==id).findFirst().orElse(null);}
}
class class_4184 {
 float field_18718,field_18717;
 void method_19325(float yaw,float pitch){field_18718=yaw;field_18717=pitch;}
}
class class_310 {
 static final class_310 INSTANCE=new class_310();
 class_746 field_1724=new class_746();
 class_638 field_1687=new class_638();
 class_1297 field_1719=field_1724;
 class_634 network=new class_634();
 class_310(){field_1687.players.add(field_1724);field_1687.players.add(new class_1657(2,"Alpha"));field_1687.players.add(new class_1657(3,"Beta"));}
 static class_310 method_1551(){return INSTANCE;}
 void method_1504(class_1297 entity){field_1719=entity;}
 class_1297 method_1560(){return field_1719;}
 class_634 method_1562(){return network;}
}
class class_634 {
 static boolean allow=true;static int commands=0;
 void method_45730(String command) {
  ++commands;if(!allow)return;
  var mc=class_310.method_1551();
  if(command.equals("gamemode spectator"))mc.field_1724.spectator=true;
  else if(command.equals("spectate"))mc.field_1719=mc.field_1724;
  else if(command.startsWith("spectate ")&&mc.field_1724.spectator) {
   var name=command.substring(9);for(var player:mc.field_1687.players)if(player.account.equals(name)){mc.field_1719=player;break;}
  }
 }
}
