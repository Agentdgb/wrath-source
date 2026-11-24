package net.anubis.client.module.impl.render.character.kit;
import java.io.*; import java.nio.file.*; import java.awt.image.BufferedImage; import javax.imageio.ImageIO;
import net.anubis.client.module.impl.render.character.designs.*;
public final class ExportNative {
 static void floats(DataOutputStream o,float[] a)throws Exception {o.writeInt(a==null?0:a.length);if(a!=null)for(float f:a)o.writeFloat(f);}
 static void surface(DataOutputStream o,Surface s)throws Exception {o.writeBoolean(s!=null);if(s==null)return; floats(o,s.pos);floats(o,s.nrm);floats(o,s.uv);o.writeInt(s.color.length);for(int c:s.color)o.writeInt(c);o.writeInt(s.quads.length);for(int q:s.quads)o.writeInt(q);floats(o,s.weight);floats(o,s.joint);o.writeInt(s.glow==null?0:s.glow.length);if(s.glow!=null)o.write(s.glow);floats(o,s.flat);}
 public static void main(String[] args)throws Exception {
  Path dir=Path.of(args[0]);Files.createDirectories(dir);
  CharacterDesign[] ds={new Sakura(),new Luna(),new Mika(),new Aoi(),new Miku(),new TungTungSahur(),new Tralalero()};
  for(int i=0;i<ds.length;i++)for(Detail d:Detail.values()) {
   Path p=dir.resolve(i+"_"+d.ordinal()+".bin");if(Files.exists(p)&&java.nio.ByteBuffer.wrap(Files.readAllBytes(p)).getInt()==0x57434832)continue;
   System.out.println("Baking "+ds[i].name()+" "+d.label);CharacterModel m=CharacterBuilder.build(ds[i],d);
   try(DataOutputStream o=new DataOutputStream(new BufferedOutputStream(Files.newOutputStream(p)))) {
    o.writeInt(0x57434832);o.writeFloat(m.scale);o.writeFloat(m.hipY);o.writeBoolean(m.firstPersonArms);o.writeInt(m.parts.length);
    for(PartMesh x:m.parts) {o.writeInt(x.bone.ordinal());o.writeInt(x.parent);o.writeBoolean(x.extra);o.writeFloat(x.pivotX);o.writeFloat(x.pivotY);o.writeFloat(x.pivotZ);o.writeFloat(x.gripX);o.writeFloat(x.gripY);o.writeFloat(x.gripZ);o.writeFloat(x.swingScale);o.writeFloat(x.swingLimit);o.writeFloat(x.jointX);o.writeFloat(x.jointY);o.writeFloat(x.jointZ);o.writeBoolean(x.neck);o.writeFloat(x.lookPitch);o.writeFloat(x.lookYaw);o.writeFloat(x.lookLimit);
     Motion q=x.motion; o.writeBoolean(q!=null);if(q!=null){for(float f:new float[]{q.restPitch,q.restYaw,q.restRoll,q.idlePitch,q.idleYaw,q.idleRoll,q.idleRate,q.idlePhase,q.walkPitch,q.walkYaw,q.walkRoll,q.walkRate,q.walkPhase,q.drag,q.hang,q.turnRoll,q.turnYaw,q.followAmount,q.followLimit,q.stiffness,q.damping,q.bend,q.limit})o.writeFloat(f);o.writeInt(q.follow==null?-1:q.follow.ordinal());}
     surface(o,x.base);surface(o,x.outline);surface(o,x.decals);
    }
   }
   if(d==Detail.LOW){BufferedImage image=new BufferedImage(m.atlasWidth,m.atlasHeight,BufferedImage.TYPE_INT_ARGB);image.setRGB(0,0,m.atlasWidth,m.atlasHeight,m.atlasMips[0],0,m.atlasWidth);ImageIO.write(image,"png",dir.resolve(i+".png").toFile());}
   System.out.println("Saved "+p+" ("+m.triangles()+" triangles)");
  }
 }
}
