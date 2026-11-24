package net.minecraft;
import dev.readableaim.*;
public final class PortReference {
 private static long randomState;
 private static SimpleAimController simple;
 private static final WindAimController wind = new WindAimController();
 private static final ManualAimOverride manual = new ManualAimOverride();
 private static double sample(double minimum, double maximum) {
  long bits = (randomState += 0x9e3779b97f4a7c15L);
  bits = (bits ^ (bits >>> 30)) * 0xbf58476d1ce4e5b9L;
  bits = (bits ^ (bits >>> 27)) * 0x94d049bb133111ebL;
  bits ^= bits >>> 31;
  double fraction = (bits >>> 11) * 0x1.0p-53;
  double result = minimum + (maximum - minimum) * fraction;
  return result < maximum ? result : Math.nextDown(maximum);
 }
 public static void reset(long seed) { randomState=seed;simple=new SimpleAimController(PortReference::sample);wind.reset();manual.reset(); }
 public static int[] simple(double x,double y,double tx,double ty,double speed,double response,boolean consume) {
  simple.updateRotation(x,y,tx,ty,speed,response);
  if(!consume)return simple.hasPendingRotation()?new int[]{1}:null;
  var result=simple.consumeRotation();return result==null?null:new int[]{result.horizontalUnits(),result.verticalUnits()};
 }
 public static double[] wind(double x,double y,double speed,double sensitivity,double seconds) {
  var result=wind.update(x,y,speed,sensitivity,seconds);return new double[]{result.horizontal(),result.vertical()};
 }
 public static boolean manual(double yawError,double pitchError,double manualYaw,double manualPitch,boolean vertical,double seconds) {
  return manual.shouldSuspend(yawError,pitchError,manualYaw,manualPitch,vertical,seconds);
 }
 public static long round(double value){return Math.round(value);}
}
