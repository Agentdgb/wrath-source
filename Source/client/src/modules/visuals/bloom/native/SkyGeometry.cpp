// Ported from ShellArrayCache: native vertex streams replace Java rendering objects.
#include "NativeImages.h"
#include "SkyModel.h"
namespace vanta::modules::bloom::sky {
Array<float> FR(24), cz;
Array<int> da;
Rotation rotation;
bool cr = true, cs = true, ct = true, bc = true, av = true, ar = true, bk = true, am = false, ay = false, an = false;
float ao = 0, ca = 1, cb = 1, ax = 1, aw = 1;
constexpr int cl = 0, cm = 1, cn = 2, co = 3, aq = 4, ba = 5, bb = 6, bj = 7;
void e(Buffers *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7, float var8,
       double var9, bool var11);
void f(Stream *var0, int var1);
void g(Stream *var0, int var1);
void x(Stream *var0, Rotation var1, float var2, int var3, int var4);
void y(Stream *var0, Array<float> var1, Array<int> var2, int var3, int var4, double var5, float var7, float var8,
       float var9);
void z(Stream *var0, Rotation var1, float var2, int var3);
void aa(Stream *var0, Rotation var1, float var2, float var3);
void ab(Stream *var0, float var1, float var2, float var3, int var4, float var5, int var6);
void ac(Stream *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7);
void ad(Stream *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7);
void ae(Stream *var0, float var1, double var2, float var4);
void af(Stream *var0, float var1, double var2, float var4, float var5, int var6, float var7);
void ag(Stream *var0, float var1, double var2, float var4, int var5);
void ah(Stream *var0, double var1, float var3);
Array<float> dg = Array<float>{0.5004883F, 0.5161133F, 0.5629883F};
Array<float> dh = Array<float>{0.5073242F, 0.5463867F, 0.6245117F};
Array<float> di = Array<float>{0.5004883F, 0.5004883F, 0.5004883F};
Array<float> dj = Array<float>{0.5073242F, 0.5307617F, 0.5620117F};
Array<float> dk = Array<float>(65);
Array<float> dl = Array<float>(1024);
Array<float> dm = Array<float>(30);
Array<float> dn = Array<float>{-0.3F, 1.3F, 13.0F, 52.0F, 1.0F, 4.2F,  0.0F,  0.55F, 1.0F,  19.0F, 60.0F, 0.8F,
                               3.4F,  1.7F, 0.05F, 1.7F,  8.0F, 32.0F, 0.55F, 5.1F,  3.1F,  -1.0F, 0.75F, 25.0F,
                               68.0F, 0.7F, 2.7F,  4.6F,  2.6F, 1.1F,  10.0F, 30.0F, 0.45F, 3.8F,  5.9F};
Array<float> _kwren_do = Array<float>{0.0F, 0.12F, 0.4F, 1.0F};
Array<float> dp = Array<float>(4);
Array<float> dq = Array<float>(4);
Array<float> dr = Array<float>(4);
Array<float> ds = Array<float>(4);
Array<float> dt = Array<float>(4);
Array<float> du = Array<float>(4);
Array<float> dv = Array<float>(4);
Array<int> dw = Array<int>(4);
Array<double> dx = Array<double>{-1.0, -1.0};
Array<float> dy = Array<float>(20);
double dz = -1.0;
Random ea = Random();

void frame(const Input &i, float angle, float radius, float pixels, float threshold) {
    for (int k = 0; k < 3; ++k) {
        FR[k] = -i.right[k];
        FR[3 + k] = i.up[k];
        FR[6 + k] = i.forward[k];
    }
    float x = 0, y = .81915206F, z = -.57357645F, c = std::cos(angle), s = std::sin(angle), v = 1 - c;
    rotation.m = {c + x * x * v,     y * x * v + z * s, z * x * v - y * s, x * y * v - z * s, c + y * y * v,
                  z * y * v + x * s, x * z * v + y * s, y * z * v - x * s, c + z * z * v};
    for (int k = 0; k < 9; ++k)
        FR[9 + k] = rotation.m[k];
    FR[18] = radius;
    FR[19] = pixels;
    FR[20] = threshold;
}
void generate(State &state, const Input &input, Buffers &buffers) {
    auto *var1 = &state;
    float var5 = input.dt;
    am = ay = an = false;
    ao = 0;
    Array<float> var6 = var1->pw;
    float var7 = var6[3];
    float var8 = var6[4];
    float var9 = var6[8];
    float var10 = var6[9];
    float var11 = var6[10];
    float var12 = var6[11];
    float var13 = var6[12];
    float var14 = var6[6];
    float var15 = var6[1];
    float var16 = var6[7];
    float var17 = Math::min(1.0F, 1.45F * var1->cv * var1->da);
    bool var18 = var1->o();
    float var19 = var18 ? var1->x() : 0.0F;
    float var20 = var17 * Math::max(Math::max(var11, var12), var14);
    float var21 = var17 * var13;
    float var22 = var17 * Math::max(var15, var16);
    if (var17 * Math::max(Math::max(var7, var8), Math::max(var9, var10)) <= 0.002F && var20 <= 0.002F &&
        var19 <= 0.002F && var21 <= 0.002F && var22 <= 0.002F) {
        return;
    }

    float var23 = var17 * Math::max(var9, var10);
    float var25 = input.underwater ? 0.0F : 1.0F;
    ca += (var25 - ca) * (1.0F - (float)Math::exp(-5.0 * input.dt));
    float var26 = input.light;
    var26 = var26 < 0.0F ? 0.0F : (var26 > 1.0F ? 1.0F : var26);
    float var27 = var17 * var7 * ca;
    float var28 = var27 * (0.25F + 0.75F * var26);
    float var29 = var27 * (0.45F + 0.55F * var26);
    float var30 = var1->dc;
    float var31 = var17 * var8 * ca * (0.15F + 0.85F * var26) * (0.7F + 0.6F * var30);
    float var32 = Math::max(Math::max(var28, 0.85F * var17 * var8 * ca * var26), 0.25F * var17 * var11 * ca * var26);
    var32 = Math::max(var32, var17 * ca * var26 * var1->dj);
    float var33 = var7 + var8 + var11;
    float var34 = var33 > 1.0E-4F ? (var7 + var8) / var33 : 1.0F;
    float var35 = var17 * var9 * ca * (0.45F + 0.55F * var26);
    float var36 = var17 * var10 * ca * (0.45F + 0.55F * var26);
    float var37 = var17 * var10 * ca * (0.35F + 0.65F * var26) * (0.6F + 0.6F * var30);
    float var38 =
        var18 ? Math::max(Math::max(var28, 0.6F * var31), var19 * ca) * (1.0F - Math::min(1.0F, var17 * var12)) : 0.0F;
    float var39 = var17 * var12 * ca * (0.3F + 0.7F * var26);
    float var40 = var22 * ca * var26;
    float var41 = var21 * ca * (0.35F + 0.65F * var26) * (0.7F + 0.45F * var30);
    if (cr) {
        cb = cb + (1.0F - cb) * (1.0F - (float)Math::exp((double)(-3.0F * var5)));
    }

    if (bc && ax < 1.0F) {
        ax = ax + (1.0F - ax) * (1.0F - (float)Math::exp((double)(-3.0F * var5)));
        ax = ax > 0.999F ? 1.0F : ax;
    }

    if (av && aw < 1.0F) {
        float var42 = aw + (1.0F - aw) * (1.0F - (float)Math::exp((double)(-3.0F * var5)));
        aw = var42 > 0.999F ? 1.0F : var42;
    }

    float var70 = 50.0F;
    float var43 = (float)input.width;
    float var44 = (float)input.height;
    if (var43 < 1 || var44 < 1)
        return;
    float var45 = input.fov;
    float var46 = var70 * 2.0F * (float)Math::tan(Math::toRadians((double)var45) * 0.5) / var44;
    double var47 = Math::tan(Math::toRadians(Math::min(170.0, (double)var45 * 1.5)) * 0.5);
    double var49 = var47 * (double)var43 / (double)var44;
    float var51 = (float)(1.0 / Math::sqrt(1.0 + var47 * var47 + var49 * var49)) - 0.05F;
    double var52 = var1->dd;
    frame(input, (float)(var52 * 8.726646259971648E-4), var70, var46, var51);
    Rotation var54 = rotation;
    float var55 = var1->di;
    int var56 = 3 + Math::round(2.0F * (var55 < 0.0F ? 0.0F : (var55 > 1.0F ? 1.0F : var55)));
    Buffers *var57 = &buffers;
    if (cr && var27 * cb > 0.002F) {
        x(var57->getBuffer(cl), var54, var70, (int)(255.0F * var27 * cb), (int)(255.0F * (0.65F + 0.35F * var26)));
    }

    if (var20 > 0.002F) {
        e(var57, var70, var17 * ca, var11, var12, var14, var26, var30, var1->er, var52, true);
    }

    Array<float> var58 = cz;
    Array<int> var59 = da;
    if (cs && var58 != nullptr && var59 != nullptr && var32 > 0.002F) {
        int var60 = (int)(1320.0F * (0.5F + 0.5F * var55));
        int var61 = var28 > 0.002F ? (int)(1080.0F * (0.5F + 0.5F * var55)) : 0;
        y(var57->getBuffer(cn), var58, var59, var60, var61, var52, var32, var28, var34);
    }

    if (ar && (var35 > 0.002F || var36 > 0.002F)) {
        float var71 = var35 > 0.002F ? var1->w() : 0.0F;
        float var74 = var36 > 0.002F ? var1->er : 0.0F;
        if (cs) {
            Stream *var62 = var57->getBuffer(cn);
            float var63 = 0.7F + 0.6F * var30;
            if (var35 > 0.002F) {
                ad(var62, var70, var71, 20.0F, 90.0F, 12.0F, 10.0F, 0.1F * var35 * var63);
                ad(var62, var70, var71, 8.5F, 150.0F, 42.0F, 20.0F, 0.16F * var35 * var63);
            }

            if (var36 > 0.002F) {
                ad(var62, var70, var74, 32.0F, 255.0F, 60.0F, 150.0F, 0.42F * var36 * var63);
                ad(var62, var70, var74, 17.0F, 255.0F, 150.0F, 80.0F, 0.3F * var36 * var63);
            }
        }

        Stream *var78 = var57->getBuffer(aq);
        if (var35 > 0.002F) {
            ab(var78, var70, var71, 5.75F, 0, 118.0F, (int)(255.0F * Math::min(1.0F, var35)));
            ao = Math::min(1.0F, var35 * 1.5F);
        }

        if (var36 > 0.002F) {
            ab(var78, var70, var74, 11.5F, 1, 120.0F, (int)(255.0F * Math::min(1.0F, var36)));
            am = true;
        }
    }

    if (cs && var29 > 0.002F) {
        z(var57->getBuffer(cm), var54, var70, (int)(255.0F * var29));
    }

    if (ct && var31 > 0.002F) {
        af(var57->getBuffer(co), var70, var52, var31, 0.5F + var1->db, var56,
           (.35F * (.5F + .5F * (float)Math::sin(var52 * .21))));
    }

    if (var20 > 0.002F) {
        e(var57, var70, var17 * ca, var11, var12, var14, var26, var30, var1->er, var52, false);
    }

    if (bk && var41 > 0.002F) {
        Array<float> var72 = FR;
        int var75 = ribbon::b(var70, var52, var41 > 2.0F ? 2.0F : var41, var72[6], var72[7], var72[8], var72[20]);
        if (var75 > 0) {
            g(var57->getBuffer(bj), var75);
        }
    }

    if (cs &&
        (var28 > 0.002F || var31 > 0.002F || var37 > 0.002F || var38 > 0.002F || var39 > 0.002F || var40 > 0.002F)) {
        Stream *var73 = var57->getBuffer(cn);
        if (var39 > 0.002F) {
            float var76 = var1->er;
            float var79 = MathHelper::cos((double)(var76 * (float)(Math::PI / 180.0))) * 4.0F + 0.5F;
            float var81 = var39 * (var79 < 0.0F ? 0.0F : (var79 > 1.0F ? 1.0F : var79));
            if (var81 > 0.004F) {
                ad(var73, var70, var76, 26.0F, 255.0F, 246.0F, 228.0F, 0.12F * var81);
                ad(var73, var70, var76, 8.0F, 255.0F, 250.0F, 238.0F, 0.38F * var81);
                ad(var73, var70, var76, 2.4F, 255.0F, 255.0F, 250.0F, var81);
                ay = true;
            }
        }

        if (var40 > 0.002F) {
            float var77 = var1->er;
            float var80 = MathHelper::cos((double)(var77 * (float)(Math::PI / 180.0))) * 8.0F + 0.5F;
            float var82 = var40 * (var80 < 0.0F ? 0.0F : (var80 > 1.0F ? 1.0F : var80)) * Math::max(var1->eu, 0.35F);
            float var64 = var15 + var16 > 1.0E-4F ? var15 / (var15 + var16) : 0.0F;
            float var65 = 1.0F - var64;
            if (var82 > 0.004F) {
                ad(var73, var70, var77, 7.0F + 2.0F * var65, 255.0F, 80.0F + 140.0F * var65, 30.0F + 140.0F * var65,
                   (0.2F * var64 + 0.16F * var65) * var82);
                ac(var73, var70, var77, 2.2F, 255.0F, 104.0F + 134.0F * var65, 44.0F + 168.0F * var65,
                   (0.85F * var64 + 0.5F * var65) * var82);
                an = true;
            }
        }

        if (var28 > 0.002F) {
            aa(var73, var54, var70, var28);
        }

        if (var31 > 0.002F) {
            ag(var73, var70, var52, var31, var56);
        }

        if (var37 > 0.002F) {
            ae(var73, var70, var52, var37);
        }

        if (var38 > 0.002F) {
            ah(var73, var52, var38);
        }
    }
}
void e(Buffers *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7, float var8,
       double var9, bool var11) {

    Array<float> var12 = FR;
    if (var11) {
        float var20 = var3 + var4 + var5;
        float var21 = var2 * var20 * ax;
        if (bc && !(var21 <= 0.002F)) {
            var21 = var21 > 1.0F ? 1.0F : var21;
            int var23 = (int)(255.0F * (0.55F + 0.45F * var6));
            float var24 = -MathHelper::sin((double)(var8 * (float)(Math::PI / 180.0)));
            Stream *var17 = var0->getBuffer(ba);
            bool var18 = true;
            if (var3 > 0.002F) {
                f(var17, strata::m(0, var1, var21, var23, var24, var12[6], var12[7], var12[8], var12[20]));
                f(var17, strata::m(1, var1, var21, var23, var24, var12[6], var12[7], var12[8], var12[20]));
                var18 = false;
            }

            if (var5 > 0.002F) {
                float var19 = var18 ? var21 : var21 * var5 / var20;
                f(var17, strata::m(3, var1, var19, var23, var24, var12[6], var12[7], var12[8], var12[20]));
                f(var17, strata::m(4, var1, var19, var23, var24, var12[6], var12[7], var12[8], var12[20]));
                var18 = false;
            }

            if (var4 > 0.002F) {
                float var25 = var18 ? var21 : var21 * var4 / var20;
                f(var17, strata::m(2, var1, var25, var23, var24, var12[6], var12[7], var12[8], var12[20]));
            }
        }
    } else {
        float var13 = aw;
        float var14 = var2 * var3 * (0.25F + 0.75F * var6) * (0.55F + 0.65F * var7) * var13;
        float var15 = var2 * var4 * (0.35F + 0.65F * var6) * var13;
        if (av && (!(var14 <= 0.002F) || !(var15 <= 0.002F))) {
            Stream *var16 = var0->getBuffer(bb);
            if (var14 > 0.002F) {
                f(var16, strata::n(var1, var9, var14, var12[6], var12[7], var12[8], var12[20]));
            }

            if (var15 > 0.002F) {
                f(var16, strata::o(var1, var9, var15, 0.55F + 0.45F * var6, var12[6], var12[7], var12[8], var12[20]));
            }
        }
    }
}
void f(Stream *var0, int var1) {

    try {
        Array<float> var2 = strata::VB;
        int var3 = var1 * 6;

        for (int var4 = 0; var4 < var3; var4 += 6) {
            var0->vertex(var2[var4], var2[var4 + 1], var2[var4 + 2])
                .texture(var2[var4 + 3], var2[var4 + 4])
                .color(Float::floatToRawIntBits(var2[var4 + 5]));
        }
    } catch (...) {
        throw;
    }
}
void g(Stream *var0, int var1) {

    try {
        Array<float> var2 = ribbon::VB;
        int var3 = var1 * 6;

        for (int var4 = 0; var4 < var3; var4 += 6) {
            var0->vertex(var2[var4], var2[var4 + 1], var2[var4 + 2])
                .texture(var2[var4 + 3], var2[var4 + 4])
                .color(Float::floatToRawIntBits(var2[var4 + 5]));
        }
    } catch (...) {
        throw;
    }
}
void x(Stream *var0, Rotation var1, float var2, int var3, int var4) {

    Array<float> var5 = span::FACE;
    float var6 = 3.2425422E-4F;
    float var7 = 4.8638132E-4F;

    for (int var8 = 0; var8 < 6; var8++) {
        int var9 = 9 * var8;
        float var10 = var5[var9] * var2;
        float var11 = var5[var9 + 1] * var2;
        float var12 = var5[var9 + 2] * var2;
        float var13 = var5[var9 + 3] * var2;
        float var14 = var5[var9 + 4] * var2;
        float var15 = var5[var9 + 5] * var2;
        float var16 = var5[var9 + 6] * var2;
        float var17 = var5[var9 + 7] * var2;
        float var18 = var5[var9 + 8] * var2;
        int var19 = var8 % 3 * (417279920 ^ 417278900) + 2;
        int var20 = var8 / 3 * (-1939715844 ^ -1939714824) + 2;
        float var21 = (float)var19 * var6;
        float var22 = (float)(var19 + (762879443 ^ 762878419)) * var6;
        float var23 = (float)var20 * var7;
        float var24 = (float)(var20 + (-265636463 ^ -265637487)) * var7;
        var0->vertex(var1, var10 - var13 + var16, var11 - var14 + var17, var12 - var15 + var18)
            .texture(var21, var23)
            .color(var4, var4, var4, var3);
        var0->vertex(var1, var10 - var13 - var16, var11 - var14 - var17, var12 - var15 - var18)
            .texture(var21, var24)
            .color(var4, var4, var4, var3);
        var0->vertex(var1, var10 + var13 - var16, var11 + var14 - var17, var12 + var15 - var18)
            .texture(var22, var24)
            .color(var4, var4, var4, var3);
        var0->vertex(var1, var10 + var13 + var16, var11 + var14 + var17, var12 + var15 + var18)
            .texture(var22, var23)
            .color(var4, var4, var4, var3);
    }
}
void y(Stream *var0, Array<float> var1, Array<int> var2, int var3, int var4, double var5, float var7, float var8,
       float var9) {

    try {
        Array<float> var10 = FR;
        float var11 = var10[0];
        float var12 = var10[1];
        float var13 = var10[2];
        float var14 = var10[3];
        float var15 = var10[4];
        float var16 = var10[5];
        float var17 = var10[6];
        float var18 = var10[7];
        float var19 = var10[8];
        float var20 = var10[9];
        float var21 = var10[10];
        float var22 = var10[11];
        float var23 = var10[12];
        float var24 = var10[13];
        float var25 = var10[14];
        float var26 = var10[15];
        float var27 = var10[16];
        float var28 = var10[17];
        float var29 = var10[18];
        float var30 = var10[19];
        float var31 = var10[20];
        int var32 = 552312589 ^ 552311333;
        int var33 = 9;
        int var34 = var32 + var4;
        Array<float> var35 = dl;
        Array<float> var36 = dk;

        for (int var37 = 0; var37 < var34; var37++) {
            if (var37 >= var3 && var37 < var32) {
                var37 = var32 - 1;
            } else {
                int var38 = var37 * var33;
                float var39 = var1[var38];
                float var40 = var1[var38 + 1];
                float var41 = var1[var38 + 2];
                float var42 = var20 * var39 + var23 * var40 + var26 * var41;
                float var43 = var21 * var39 + var24 * var40 + var27 * var41;
                float var44 = var22 * var39 + var25 * var40 + var28 * var41;
                if (!(var42 * var17 + var43 * var18 + var44 * var19 < var31) && !(var43 < -0.02F)) {
                    float var45 = var43 < 0.04F ? 0.04F : var43;
                    float var46 = 1.0F - var45;
                    float var47 = (1.0F / var45 - 1.0F) * 2.625F;
                    int var48 = (int)var47;
                    var48 = var48 > 63 ? 63 : var48;
                    float var49 = var36[var48] + (var36[var48 + 1] - var36[var48]) * (var47 - (float)var48);
                    float var50 = var1[var38 + 5] * (0.15F + 0.85F * var46 * var46 * var46);
                    float var51 =
                        var1[var38 + 4] * var49 *
                        (1.0F - var50 +
                         var50 *
                             (0.5F + 0.5F * MathHelper::sin(var5 * (double)var1[var38 + 6] + (double)var1[var38 + 7])));
                    float var52 =
                        var35[(int)((var51 > 1.0F ? 1.0F : var51) * 1023.0F + 0.5F)] * (var37 < var32 ? var7 : var8);
                    if (!(var52 < 0.012F)) {
                        if (var52 > 1.0F) {
                            var52 = 1.0F;
                        }

                        int var53 = (int)var1[var38 + 8];
                        if (var53 == 2 && var9 < 1.0F) {
                            var52 *= var9;
                            if (var52 < 0.012F) {
                                continue;
                            }
                        }

                        float var54 = var1[var38 + 3] * var30 * (var53 == 2 ? 3.5F : 1.0F);
                        float var55 = var42 * var29;
                        float var56 = var43 * var29;
                        float var57 = var44 * var29;
                        float var58 = var11 * var54;
                        float var59 = var12 * var54;
                        float var60 = var13 * var54;
                        float var61 = var14 * var54;
                        float var62 = var15 * var54;
                        float var63 = var16 * var54;
                        int var64 = var2[var37];
                        int var65 = (int)((float)(var64 >> 16 & (1246377235 ^ 1246377452)) * var52);
                        int var66 = (int)((float)(var64 >> 8 & (1389221834 ^ 1389221685)) * var52);
                        int var67 = (int)((float)(var64 & (-79967169 ^ -79967040)) * var52);
                        float var68 = dg[var53];
                        float var69 = dh[var53];
                        float var70 = di[var53];
                        float var71 = dj[var53];
                        var0->vertex(var55 - var58 + var61, var56 - var59 + var62, var57 - var60 + var63)
                            .texture(var68, var70)
                            .color(var65, var66, var67, 1960410199 ^ 1960410280);
                        var0->vertex(var55 - var58 - var61, var56 - var59 - var62, var57 - var60 - var63)
                            .texture(var68, var71)
                            .color(var65, var66, var67, -1093679956 ^ -1093680045);
                        var0->vertex(var55 + var58 - var61, var56 + var59 - var62, var57 + var60 - var63)
                            .texture(var69, var71)
                            .color(var65, var66, var67, 1268441480 ^ 1268441463);
                        var0->vertex(var55 + var58 + var61, var56 + var59 + var62, var57 + var60 + var63)
                            .texture(var69, var70)
                            .color(var65, var66, var67, 494121531 ^ 494121668);
                    }
                }
            }
        }
    } catch (...) {
        throw;
    }
}
void z(Stream *var0, Rotation var1, float var2, int var3) {

    Array<float> var4 = dm;

    for (int var5 = 0; var5 < 30; var5 += 15) {
        float var6 = var4[var5] * var2;
        float var7 = var4[var5 + 1] * var2;
        float var8 = var4[var5 + 2] * var2;
        float var9 = var4[var5 + 9] * var2;
        float var10 = var4[var5 + 10] * var2;
        float var11 = var4[var5 + 3] * var9;
        float var12 = var4[var5 + 4] * var9;
        float var13 = var4[var5 + 5] * var9;
        float var14 = var4[var5 + 6] * var10;
        float var15 = var4[var5 + 7] * var10;
        float var16 = var4[var5 + 8] * var10;
        float var17 = var4[var5 + 11];
        float var18 = var4[var5 + 12];
        float var19 = var4[var5 + 13];
        float var20 = var4[var5 + 14];
        var0->vertex(var1, var6 - var11 + var14, var7 - var12 + var15, var8 - var13 + var16)
            .texture(var17, var18)
            .color(var3, var3, var3, var3);
        var0->vertex(var1, var6 + var11 + var14, var7 + var12 + var15, var8 + var13 + var16)
            .texture(var19, var18)
            .color(var3, var3, var3, var3);
        var0->vertex(var1, var6 + var11 - var14, var7 + var12 - var15, var8 + var13 - var16)
            .texture(var19, var20)
            .color(var3, var3, var3, var3);
        var0->vertex(var1, var6 - var11 - var14, var7 - var12 - var15, var8 - var13 - var16)
            .texture(var17, var20)
            .color(var3, var3, var3, var3);
    }
}
void aa(Stream *var0, Rotation var1, float var2, float var3) {

    Array<float> var4 = dm;
    float var5 = 0.41F * var2;
    float var6 = var4[0] * var2;
    float var7 = var4[1] * var2;
    float var8 = var4[2] * var2;
    float var9 = var4[3] * var5;
    float var10 = var4[4] * var5;
    float var11 = var4[5] * var5;
    float var12 = var4[6] * var5;
    float var13 = var4[7] * var5;
    float var14 = var4[8] * var5;
    int var15 = (int)(11.0F * var3);
    int var16 = (int)(14.0F * var3);
    int var17 = (int)(25.5F * var3);
    float var18 = 0.25048828F;
    float var19 = 0.49951172F;
    float var20 = 0.5004883F;
    float var21 = 0.7495117F;
    var0->vertex(var1, var6 - var9 + var12, var7 - var10 + var13, var8 - var11 + var14)
        .texture(var18, var20)
        .color(var15, var16, var17, 2024779352 ^ 2024779431);
    var0->vertex(var1, var6 + var9 + var12, var7 + var10 + var13, var8 + var11 + var14)
        .texture(var19, var20)
        .color(var15, var16, var17, -558643980 ^ -558644213);
    var0->vertex(var1, var6 + var9 - var12, var7 + var10 - var13, var8 + var11 - var14)
        .texture(var19, var21)
        .color(var15, var16, var17, 221779136 ^ 221779007);
    var0->vertex(var1, var6 - var9 - var12, var7 - var10 - var13, var8 - var11 - var14)
        .texture(var18, var21)
        .color(var15, var16, var17, -1737516492 ^ -1737516341);
}
void ab(Stream *var0, float var1, float var2, float var3, int var4, float var5, int var6) {

    float var7 = var2 * (float)(Math::PI / 180.0);
    float var8 = -MathHelper::sin((double)var7);
    float var9 = MathHelper::cos((double)var7);
    float var10 = var8 >= 0.0F ? 1.0F : -1.0F;
    float var11 = (float)Math::tan((double)(var3 * (float)(Math::PI / 180.0))) * var1 * (128.0F / var5);
    float var12 = var8 * var1;
    float var13 = var9 * var1;
    float var14 = var10 * var11;
    float var15 = -var10 * var9 * var11;
    float var16 = var10 * var8 * var11;
    float var17 = ((float)(var4 * (239288248 ^ 239287992)) + 0.5F) / 512.0F;
    float var18 = ((float)(var4 * (-472989193 ^ -472989449)) + 255.5F) / 512.0F;
    float var19 = 0.001953125F;
    float var20 = 0.9980469F;
    var0->vertex(var12 + var15, var13 + var16, -var14).texture(var17, var19).color(var6, var6, var6, var6);
    var0->vertex(var12 + var15, var13 + var16, var14).texture(var18, var19).color(var6, var6, var6, var6);
    var0->vertex(var12 - var15, var13 - var16, var14).texture(var18, var20).color(var6, var6, var6, var6);
    var0->vertex(var12 - var15, var13 - var16, -var14).texture(var17, var20).color(var6, var6, var6, var6);
}
void ac(Stream *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7) {

    if (!(var7 <= 0.004F)) {
        float var8 = var2 * (float)(Math::PI / 180.0);
        float var9 = -MathHelper::sin((double)var8);
        float var10 = MathHelper::cos((double)var8);
        float var11 = var9 >= 0.0F ? 1.0F : -1.0F;
        float var12 = (float)Math::tan((double)(var3 * (float)(Math::PI / 180.0))) * var1 * 1.1627907F;
        float var13 = var9 * var1;
        float var14 = var10 * var1;
        float var15 = var11 * var12;
        float var16 = -var11 * var10 * var12;
        float var17 = var11 * var9 * var12;
        int var18 = (int)Math::min(255.0F, var4 * var7);
        int var19 = (int)Math::min(255.0F, var5 * var7);
        int var20 = (int)Math::min(255.0F, var6 * var7);
        float var21 = 0.7504883F;
        float var22 = 0.9995117F;
        float var23 = 0.5004883F;
        float var24 = 0.7495117F;
        var0->vertex(var13 + var16, var14 + var17, -var15)
            .texture(var21, var23)
            .color(var18, var19, var20, 2041433753 ^ 2041433702);
        var0->vertex(var13 + var16, var14 + var17, var15)
            .texture(var22, var23)
            .color(var18, var19, var20, -339394609 ^ -339394768);
        var0->vertex(var13 - var16, var14 - var17, var15)
            .texture(var22, var24)
            .color(var18, var19, var20, 280428885 ^ 280428970);
        var0->vertex(var13 - var16, var14 - var17, -var15)
            .texture(var21, var24)
            .color(var18, var19, var20, -224702209 ^ -224702464);
    }
}
void ad(Stream *var0, float var1, float var2, float var3, float var4, float var5, float var6, float var7) {

    if (!(var7 <= 0.004F)) {
        float var8 = var2 * (float)(Math::PI / 180.0);
        float var9 = -MathHelper::sin((double)var8);
        float var10 = MathHelper::cos((double)var8);
        float var11 = var9 >= 0.0F ? 1.0F : -1.0F;
        float var12 = (float)Math::tan((double)(Math::min(70.0F, var3) * (float)(Math::PI / 180.0))) * var1;
        float var13 = var9 * var1;
        float var14 = var10 * var1;
        float var15 = var11 * var12;
        float var16 = -var11 * var10 * var12;
        float var17 = var11 * var9 * var12;
        int var18 = (int)Math::min(255.0F, var4 * var7);
        int var19 = (int)Math::min(255.0F, var5 * var7);
        int var20 = (int)Math::min(255.0F, var6 * var7);
        float var21 = 0.25048828F;
        float var22 = 0.49951172F;
        float var23 = 0.5004883F;
        float var24 = 0.7495117F;
        var0->vertex(var13 + var16, var14 + var17, -var15)
            .texture(var21, var23)
            .color(var18, var19, var20, -345971951 ^ -345971730);
        var0->vertex(var13 + var16, var14 + var17, var15)
            .texture(var22, var23)
            .color(var18, var19, var20, -281916259 ^ -281916318);
        var0->vertex(var13 - var16, var14 - var17, var15)
            .texture(var22, var24)
            .color(var18, var19, var20, 1176860233 ^ 1176860342);
        var0->vertex(var13 - var16, var14 - var17, -var15)
            .texture(var21, var24)
            .color(var18, var19, var20, 444789189 ^ 444789050);
    }
}
void ae(Stream *var0, float var1, double var2, float var4) {

    try {
        float var5 = var4 * (0.85F + 0.15F * MathHelper::sin(var2 * 0.8));
        int var6 = (int)Math::min(255.0F, 158.1F * var5);
        int var7 = (int)Math::min(255.0F, 37.2F * var5);
        int var8 = (int)Math::min(255.0F, 105.4F * var5);
        float var9 = -0.0698F;
        float var10 = 0.2094F;
        float var11 = MathHelper::cos((double)var9);
        float var12 = MathHelper::sin((double)var9);
        float var13 = MathHelper::cos((double)var10);
        float var14 = MathHelper::sin((double)var10);
        float var15 = 0.37451172F;
        float var16 = 0.6245117F;
        float var17 = 0.0F;
        float var18 = 0.0F;

        for (int var19 = 0; var19 <= 48; var19++) {
            float var20 = (float)var19 * 0.1308997F;
            float var21 = MathHelper::sin((double)var20) * var1;
            float var22 = -MathHelper::cos((double)var20) * var1;
            if (var19 > 0) {
                var0->vertex(var17 * var11, var12 * var1, var18 * var11)
                    .texture(var15, var16)
                    .color(0, 0, 0, 2098491578 ^ 2098491461);
                var0->vertex(var21 * var11, var12 * var1, var22 * var11)
                    .texture(var15, var16)
                    .color(0, 0, 0, 1011664088 ^ 1011663911);
                var0->vertex(var21, 0.0F, var22).texture(var15, var16).color(var6, var7, var8, -33993997 ^ -33994228);
                var0->vertex(var17, 0.0F, var18).texture(var15, var16).color(var6, var7, var8, 1558102255 ^ 1558102032);
                var0->vertex(var17, 0.0F, var18).texture(var15, var16).color(var6, var7, var8, 596993546 ^ 596993781);
                var0->vertex(var21, 0.0F, var22).texture(var15, var16).color(var6, var7, var8, 1475298850 ^ 1475299037);
                var0->vertex(var21 * var13, var14 * var1, var22 * var13)
                    .texture(var15, var16)
                    .color(0, 0, 0, 1005218091 ^ 1005218260);
                var0->vertex(var17 * var13, var14 * var1, var18 * var13)
                    .texture(var15, var16)
                    .color(0, 0, 0, 1098726082 ^ 1098725949);
            }

            var17 = var21;
            var18 = var22;
        }
    } catch (...) {
        throw;
    }
}
void af(Stream *var0, float var1, double var2, float var4, float var5, int var6, float var7) {

    try {
        double var8 = var2 * (double)var5;
        Array<float> var10 = dq;
        Array<float> var11 = dr;
        Array<float> var12 = ds;
        Array<float> var13 = dt;
        Array<float> var14 = du;
        Array<float> var15 = dv;
        Array<float> var16 = _kwren_do;
        Array<float> var17 = dp;
        Array<float> var18 = dn;
        Array<int> var19 = dw;

        for (int var20 = 0; var20 < 4; var20++) {
            var19[var20] = (int)(255.0F * (1.0F - var7 * (0.3F + 0.9F * var16[var20])));
        }

        float var61 = (float)(Math::PI * 2);

        for (int var21 = 0; var21 < var6 && var21 < 5; var21++) {
            int var22 = var21 * 7;
            float var23 = var18[var22];
            float var24 = var18[var22 + 1];
            float var25 = var18[var22 + 2];
            float var26 = var18[var22 + 3];
            float var27 = var18[var22 + 4];
            float var28 = var18[var22 + 5];
            float var29 = var18[var22 + 6];
            float var30 = 0.55F + 0.45F * (0.5F + 0.5F * MathHelper::sin(0.31 * var2 + (double)var29)) *
                                      (0.6F + 0.4F * MathHelper::sin(0.83 * var2 + 2.0 * (double)var29));
            float var31 = var4 * var27 * var30;
            if (!(var31 < 0.004F)) {
                float var32 = (float)(std::fmod(0.012 * var8 + .1 * var29, 64.0));
                float var33 = 0.0F;
                int var34 = 0;

                for (int var35 = 0; var35 <= 56; var35++) {
                    float var36 = (float)var35 / 56.0F;
                    double var37 = (double)(var61 * 1.3F * var36) + 0.22 * var8 + (double)var29;
                    double var39 = (double)(var61 * 3.7F * var36) - 0.47 * var8 + (double)(1.3F * var29);
                    float var41 = MathHelper::sin(var37);
                    float var42 = MathHelper::sin(var39);
                    float var43 = var23 + (var36 - 0.5F) * var24 + 0.045F * var41 + 0.018F * var42;
                    float var44 = var24 + 0.3676F * MathHelper::cos(var37) + 0.4185F * MathHelper::cos(var39);
                    float var45 = Math::abs(var44);
                    float var46 = var24 / (var45 < 0.05F ? 0.05F : var45);
                    var46 = var46 < 0.5F ? 0.5F : (var46 > 2.2F ? 2.2F : var46);
                    float var47 = var25 + 0.020944F * MathHelper::sin((double)(var61 * 2.3F * var36) + 0.35 * var8 +
                                                                      (double)var29);
                    float var48 = var26 +
                                  0.087266F * MathHelper::sin((double)(var61 * 1.1F * var36) - 0.19 * var8 +
                                                              (double)(2.0F * var29)) +
                                  0.043633F * MathHelper::sin((double)(var61 * 4.3F * var36) + 0.41 * var8);
                    float var49 = var36 * 8.333334F;
                    float var50 = (1.0F - var36) * 8.333334F;
                    var49 = var49 >= 1.0F ? 1.0F : var49;
                    var50 = var50 >= 1.0F ? 1.0F : var50;
                    float var51 = var49 * var49 * (3.0F - 2.0F * var49) * var50 * var50 * (3.0F - 2.0F * var50);
                    float var52 =
                        var31 * var51 *
                        (0.6F + 0.4F * MathHelper::sin((double)(var61 * 2.0F * var36) - 0.6 * var8 + (double)var29)) *
                        (0.55F + 0.45F * var46);
                    int var53 = (int)(255.0F * (var52 > 1.0F ? 1.0F : var52));
                    float var54 = var36 * var28 + var32;
                    float var55 = MathHelper::sin((double)var43);
                    float var56 = MathHelper::cos((double)var43);

                    for (int var57 = 0; var57 < 4; var57++) {
                        float var58 = var47 + var16[var57] * (var48 - var47);
                        float var59 = MathHelper::cos((double)var58);
                        var13[var57] = var59 * var55 * var1;
                        var14[var57] = MathHelper::sin((double)var58) * var1;
                        var15[var57] = -var59 * var56 * var1;
                    }

                    if (var35 > 0) {
                        for (int var65 = 0; var65 < 3; var65++) {
                            var0->vertex(var10[var65], var11[var65], var12[var65])
                                .texture(var33, var17[var65])
                                .color(-581515574 ^ -581515723, var19[var65], -900616933 ^ -900616732, var34);
                            var0->vertex(var10[var65 + 1], var11[var65 + 1], var12[var65 + 1])
                                .texture(var33, var17[var65 + 1])
                                .color(1206868782 ^ 1206868945, var19[var65 + 1], -1920468585 ^ -1920468632, var34);
                            var0->vertex(var13[var65 + 1], var14[var65 + 1], var15[var65 + 1])
                                .texture(var54, var17[var65 + 1])
                                .color(-447837737 ^ -447837912, var19[var65 + 1], 1046187784 ^ 1046188023, var53);
                            var0->vertex(var13[var65], var14[var65], var15[var65])
                                .texture(var54, var17[var65])
                                .color(60361666 ^ 60361533, var19[var65], 590258510 ^ 590258609, var53);
                        }
                    }

                    for (int var66 = 0; var66 < 4; var66++) {
                        var10[var66] = var13[var66];
                        var11[var66] = var14[var66];
                        var12[var66] = var15[var66];
                    }

                    var33 = var54;
                    var34 = var53;
                }
            }
        }
    } catch (...) {
        throw;
    }
}
void ag(Stream *var0, float var1, double var2, float var4, int var5) {

    try {
        Array<float> var6 = dn;
        float var7 = 0.25048828F;
        float var8 = 0.49951172F;
        float var9 = 0.5004883F;
        float var10 = 0.7495117F;
        float var11 = 0.7F * var1;
        float var12 = 0.12F * var1;

        for (int var13 = 0; var13 < var5 && var13 < 5; var13++) {
            int var14 = var13 * 7;
            float var15 = var6[var14 + 6];
            float var16 = 0.55F + 0.45F * (0.5F + 0.5F * MathHelper::sin(0.31 * var2 + (double)var15)) *
                                      (0.6F + 0.4F * MathHelper::sin(0.83 * var2 + 2.0 * (double)var15));
            float var17 = 0.25F * var4 * var16;
            if (!(var17 < 0.004F)) {
                float var18 = var6[var14];
                float var19 = var6[var14 + 2] - 0.06981317F;
                float var20 = MathHelper::cos((double)var19);
                float var21 = MathHelper::sin((double)var19);
                float var22 = MathHelper::sin((double)var18);
                float var23 = MathHelper::cos((double)var18);
                float var24 = var20 * var22 * var1;
                float var25 = var21 * var1;
                float var26 = -var20 * var23 * var1;
                float var27 = var23 * var11;
                float var28 = var22 * var11;
                float var29 = -var22 * var21 * var12;
                float var30 = var20 * var12;
                float var31 = var23 * var21 * var12;
                int var32 = (int)(40.0F * var17);
                int var33 = (int)(180.0F * var17);
                int var34 = (int)(110.0F * var17);
                var0->vertex(var24 - var27 + var29, var25 + var30, var26 - var28 + var31)
                    .texture(var7, var9)
                    .color(var32, var33, var34, -603482814 ^ -603482691);
                var0->vertex(var24 - var27 - var29, var25 - var30, var26 - var28 - var31)
                    .texture(var7, var10)
                    .color(var32, var33, var34, 394131433 ^ 394131222);
                var0->vertex(var24 + var27 - var29, var25 - var30, var26 + var28 - var31)
                    .texture(var8, var10)
                    .color(var32, var33, var34, -1080683380 ^ -1080683405);
                var0->vertex(var24 + var27 + var29, var25 + var30, var26 + var28 + var31)
                    .texture(var8, var9)
                    .color(var32, var33, var34, -204827498 ^ -204827543);
            }
        }
    } catch (...) {
        throw;
    }
}
void ah(Stream *var0, double var1, float var3) {

    if (dz < 0.0 || dz > var1 + 81.0) {
        dz = var1 + 8.0 + 20.0 * ea.nextDouble();
    }

    if (var1 >= dz) {
        dz = var1 + 20.0 + 60.0 * ea.nextDouble();
        int var4 = dx[0] < 0.0 ? 0 : (dx[1] < 0.0 ? 1 : -1);
        if (var4 >= 0) {
            double var5 = ea.nextDouble() * Math::PI * 2.0;
            double var7 = Math::toRadians(25.0 + 50.0 * ea.nextDouble());
            double var9 = Math::cos(var7) * Math::sin(var5);
            double var11 = Math::sin(var7);
            double var13 = -Math::cos(var7) * Math::cos(var5);
            double var15 = var9 * var11;
            double var17 = var11 * var11 - 1.0;
            double var19 = var13 * var11;
            double var21 = Math::sqrt(var15 * var15 + var17 * var17 + var19 * var19);
            var15 /= var21;
            var17 /= var21;
            var19 /= var21;
            double var23 = var11 * var19 - var13 * var17;
            double var25 = var13 * var15 - var9 * var19;
            double var27 = var9 * var17 - var11 * var15;
            double var29 = (ea.nextDouble() * 2.0 - 1.0) * 1.05;
            double var31 = Math::cos(var29);
            double var33 = Math::sin(var29);
            int var35 = var4 * 10;
            dy[var35] = (float)var9;
            dy[var35 + 1] = (float)var11;
            dy[var35 + 2] = (float)var13;
            dy[var35 + 3] = (float)(var15 * var31 + var23 * var33);
            dy[var35 + 4] = (float)(var17 * var31 + var25 * var33);
            dy[var35 + 5] = (float)(var19 * var31 + var27 * var33);
            dy[var35 + 6] = (float)Math::toRadians(18.0 + 17.0 * ea.nextDouble());
            dy[var35 + 7] = (float)(0.5 + 0.6 * ea.nextDouble());
            dy[var35 + 8] = (float)Math::toRadians(7.0 + 7.0 * ea.nextDouble());
            dy[var35 + 9] = (float)(0.6 + 0.4 * ea.nextDouble());
            dx[var4] = var1;
        }
    }

    Array<float> var49 = FR;
    float var50 = var49[18];
    float var6 = var49[19];

    for (int var51 = 0; var51 < 2; var51++) {
        if (!(dx[var51] < 0.0)) {
            int var8 = var51 * 10;
            float var52 = (float)(var1 - dx[var51]);
            float var10 = dy[var8 + 7];
            if (!(var52 < 0.0F) && !(var52 >= var10)) {
                float var53 = MathHelper::sin((double)((float)Math::PI * var52 / var10));
                var53 = (float)Math::sqrt(var53 > 0.0F ? (double)var53 : 0.0) * dy[var8 + 9] * var3;
                if (!(var53 < 0.004F)) {
                    float var12 = dy[var8 + 6];
                    float var55 = dy[var8 + 8];
                    float var14 = var12 * var52;
                    float var57 = Math::min(var55, 1.6F * var12 * var52);
                    float var16 = MathHelper::cos((double)var14);
                    float var59 = MathHelper::sin((double)var14);
                    float var18 = MathHelper::cos((double)(var14 - var57));
                    float var61 = MathHelper::sin((double)(var14 - var57));
                    float var20 = dy[var8] * var16 + dy[var8 + 3] * var59;
                    float var62 = dy[var8 + 1] * var16 + dy[var8 + 4] * var59;
                    float var22 = dy[var8 + 2] * var16 + dy[var8 + 5] * var59;
                    float var63 = dy[var8] * var18 + dy[var8 + 3] * var61;
                    float var24 = dy[var8 + 1] * var18 + dy[var8 + 4] * var61;
                    float var64 = dy[var8 + 2] * var18 + dy[var8 + 5] * var61;
                    float var26 = (var20 + var63) * 0.5F * var50;
                    float var65 = (var62 + var24) * 0.5F * var50;
                    float var28 = (var22 + var64) * 0.5F * var50;
                    float var66 = (var20 - var63) * 0.5F * var50;
                    float var30 = (var62 - var24) * 0.5F * var50;
                    float var67 = (var22 - var64) * 0.5F * var50;
                    float var32 = var65 * var67 - var28 * var30;
                    float var69 = var28 * var66 - var26 * var67;
                    float var34 = var26 * var30 - var65 * var66;
                    float var72 = (float)Math::sqrt((double)(var32 * var32 + var69 * var69 + var34 * var34));
                    int var36 = (int)(255.0F * var53);
                    int var37 = (int)(245.0F * var53);
                    int var38 = (int)(230.0F * var53);
                    if (var72 > 1.0E-6F) {
                        float var39 = 0.0021F * var50 / var72;
                        var32 *= var39;
                        var69 *= var39;
                        var34 *= var39;
                        float var40 = 0.5004883F;
                        float var41 = 0.7495117F;
                        float var42 = 0.5629883F;
                        float var43 = 0.5932617F;
                        var0->vertex(var26 - var66 + var32, var65 - var30 + var69, var28 - var67 + var34)
                            .texture(var40, var42)
                            .color(var36, var37, var38, -2042814112 ^ -2042814049);
                        var0->vertex(var26 - var66 - var32, var65 - var30 - var69, var28 - var67 - var34)
                            .texture(var40, var43)
                            .color(var36, var37, var38, -858378098 ^ -858378127);
                        var0->vertex(var26 + var66 - var32, var65 + var30 - var69, var28 + var67 - var34)
                            .texture(var41, var43)
                            .color(var36, var37, var38, -1105841916 ^ -1105841669);
                        var0->vertex(var26 + var66 + var32, var65 + var30 + var69, var28 + var67 + var34)
                            .texture(var41, var42)
                            .color(var36, var37, var38, -636612406 ^ -636612555);
                    }

                    float var73 = 3.5F * var6;
                    float var74 = var20 * var50;
                    float var75 = var62 * var50;
                    float var76 = var22 * var50;
                    float var77 = var49[0] * var73;
                    float var44 = var49[1] * var73;
                    float var45 = var49[2] * var73;
                    float var46 = var49[3] * var73;
                    float var47 = var49[4] * var73;
                    float var48 = var49[5] * var73;
                    var0->vertex(var74 - var77 + var46, var75 - var44 + var47, var76 - var45 + var48)
                        .texture(dg[1], di[1])
                        .color(var36, var37, var38, -1378790244 ^ -1378790301);
                    var0->vertex(var74 - var77 - var46, var75 - var44 - var47, var76 - var45 - var48)
                        .texture(dg[1], dj[1])
                        .color(var36, var37, var38, 1266615643 ^ 1266615716);
                    var0->vertex(var74 + var77 - var46, var75 + var44 - var47, var76 + var45 - var48)
                        .texture(dh[1], dj[1])
                        .color(var36, var37, var38, -222123167 ^ -222123106);
                    var0->vertex(var74 + var77 + var46, var75 + var44 + var47, var76 + var45 + var48)
                        .texture(dh[1], di[1])
                        .color(var36, var37, var38, 834872240 ^ 834872143);
                }
            } else {
                dx[var51] = -1.0;
            }
        }
    }
}
const bool initialized = []() {
    for (int var0 = 0; var0 < 5; var0++) {
        dn[var0 * 7 + 2] = (float)Math::toRadians((double)dn[var0 * 7 + 2]);
        dn[var0 * 7 + 3] = (float)Math::toRadians((double)dn[var0 * 7 + 3]);
    }

    for (int var34 = 0; var34 < 4; var34++) {
        dp[var34] = (0.5F + (1.0F - _kwren_do[var34]) * 255.0F) / 256.0F;
    }

    for (int var35 = 0; var35 < 1024; var35++) {
        double var1 = 0.9 * (double)var35 / 1023.0;
        dl[var35] = (float)Math::pow(var1 / (1.0 + 0.35 * var1), 0.45454545454545453);
    }

    for (int var36 = 0; var36 < 65; var36++) {
        dk[var36] = (float)Math::exp(-0.23 * (double)var36 * 0.38095238095238093);
    }

    Array<double> var37 = Array<double>{48.0, 76.0};
    Array<double> var38 = Array<double>{30.0, 47.0};
    Array<double> var2 = Array<double>{9.0, 2.4};
    Array<double> var3 = Array<double>{512.0, 128.0};
    Array<double> var4 = Array<double>{256.0, 128.0};
    Array<double> var5 = Array<double>{176.0, 120.0};
    Array<float> var6 = Array<float>{0.5F, 0.5F, 1023.5F, 511.5F, 0.5F, 512.5F, 255.5F, 767.5F};

    for (int var7 = 0; var7 < 2; var7++) {
        double var8 = Math::toRadians(var37[var7]);
        double var10 = Math::toRadians(var38[var7]);
        double var12 = Math::cos(var10) * Math::sin(var8);
        double var14 = Math::sin(var10);
        double var16 = -Math::cos(var10) * Math::cos(var8);
        double var18 = -var16;
        double var22 = Math::sqrt(var18 * var18 + var12 * var12);
        var18 /= var22;
        double var20 = var12 / var22;
        double var24 = -var20 * var14;
        double var26 = var20 * var12 - var18 * var16;
        double var28 = var18 * var14;
        double var30 = Math::tan(Math::toRadians(var2[var7])) / var5[var7];
        int var32 = var7 * 15;
        dm[var32] = (float)var12;
        dm[var32 + 1] = (float)var14;
        dm[var32 + 2] = (float)var16;
        dm[var32 + 3] = (float)var18;
        dm[var32 + 4] = 0.0F;
        dm[var32 + 5] = (float)var20;
        dm[var32 + 6] = (float)var24;
        dm[var32 + 7] = (float)var26;
        dm[var32 + 8] = (float)var28;
        dm[var32 + 9] = (float)(var30 * var3[var7]);
        dm[var32 + 10] = (float)(var30 * var4[var7]);

        for (int var33 = 0; var33 < 4; var33++) {
            dm[var32 + 11 + var33] = var6[var7 * 4 + var33] / 1024.0F;
        }
    }
    return true;
}();
void reset() {
    ca = 1;
    dz = -1;
    dx[0] = dx[1] = -1;
    am = ay = an = false;
    ao = 0;
}
float moonCoverage() { return ao; }
float sunCoverage() { return an || ay ? 1.f : 0.f; }
void starData(Array<float> positions, Array<int> colors) {
    cz = positions;
    da = colors;
}
} // namespace vanta::modules::bloom::sky
