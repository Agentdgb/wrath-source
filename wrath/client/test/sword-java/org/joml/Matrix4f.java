package org.joml;
public final class Matrix4f {
    public float[] get(float[] v) { java.util.Arrays.fill(v,0);v[0]=v[5]=v[10]=v[15]=1;v[12]=2;return v; }
}
