// Ported from Bloom 2.2; numerical source preserved. Native C++ only.
#include "NativeImages.h"
namespace vanta::modules::bloom::ribbon {

int W = 256;
int H = 1024;
int STRIDE = 6;
int f = 128;
float g = 3.0F;
float h = 2.6F;
int i = 128;
int j = 895;
float k = 24.0F;
float l = 0.42F;
float m = 0.4F;
float n = 1.0F;
float o = 0.9F;
float p = 2.8F;
float q = 3.2F;
float r = 64.0F;
float s = 48.0F;
float t = 64.0F;
Array<float> u = Array<float>{70.0F, 223.0F, 235.0F, 6.0F, 110.0F, 200.0F, 6.0F, 133.0F, 242.0F, 0.0F, 136.0F, 247.0F};
Array<float> v = Array<float>{-150.0F, 300.0F, 25.0F, 400.0F, 115.0F, 130.0F, 10.0F, 176.0F};
Array<float> w = Array<float>{
    3.2F,  7.0F, 0.068F, 1.03F, 1.6F, 11.0F, -0.105F, 2.1F, 0.8F, 19.0F, 0.165F, 5.0F, 0.0F, 1.0F,  0.0F,   0.0F,
    11.0F, 2.2F, 0.047F, 1.3F,  6.5F, 7.0F,  0.068F,  2.6F, 2.6F, 13.0F, 0.135F, 4.0F, 1.0F, 23.0F, -0.21F, 1.1F,
    2.0F,  3.5F, -0.08F, 0.33F, 0.8F, 7.0F,  0.12F,   1.9F, 0.0F, 1.0F,  0.0F,   0.0F, 0.0F, 1.0F,  0.0F,   0.0F,
    3.0F,  1.3F, 0.04F,  2.2F,  2.4F, 3.5F,  -0.08F,  1.9F, 1.0F, 7.0F,  0.15F,  3.3F, 0.0F, 1.0F,  0.0F,   0.0F};
Array<float> x = Array<float>{0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.4F, 0.0F, 0.0F};
int y = 75;
float z = -3.0F;
float aa = (float)(Math::PI * 2);
float ab = (float)(Math::PI / 180.0);
float ac = (float)(180.0 / Math::PI);
double ad = Math::PI * 2;
Array<float> ae = Array<float>(65536);
float af = 10430.378F;
int ag = 401;
Array<float> ah = Array<float>(401);
Array<float> ai = Array<float>(401);
Array<float> aj = Array<float>(401);
Array<float> ak = Array<float>(401);
Array<float> al = Array<float>(401);
Array<float> am = Array<float>(401);
Array<float> an = Array<float>(401);
Array<float> ao = Array<float>(401);
Array<float> ap = Array<float>(401);
Array<float> aq = Array<float>(401);
Array<float> ar = Array<float>(401);
Array<float> as = Array<float>(401);
Array<float> at = Array<float>(401);
Array<float> au = Array<float>(8);
Array<float> av = Array<float>(8);
Array<float> aw = Array<float>(8);
Array<float> VB = Array<float>(84000);
float ax = 1.9F;
Random ay = Random();
double az = -1.0;
double ba = -1.0;
float bb = 0.5F;
float bc = 1.0F;
int bd = 0;

Array<int> a() {

    try {
        Array<int> var0 = Array<int>(262144);
        Array<float> var1 = Array<float>(256);
        Random var2 = Random(1311792877L);
        double var3 = 63.5;

        for (int var5 = 0; var5 < (-1471084477 ^ -1471083453); var5++) {
            if (var5 < (266997605 ^ 266997733)) {
                for (int var21 = 0; var21 < (-1944470961 ^ -1944470705); var21++) {
                    int var7 = var21 < (-1661902522 ^ -1661902394) ? 0 : 1;
                    double var25 = ((double)(var21 - var7 * (1189705560 ^ 1189705688)) - var3) / var3;
                    double var27 = ((double)var5 - var3) / var3;
                    double var29 = Math::sqrt(var25 * var25 + var27 * var27);
                    double var30 = var29 * (double)(var7 == 0 ? 3.0F : 2.6F);
                    double var16 = (var29 - 0.8) / 0.2;
                    double var18 = var29 < 0.8 ? 1.0 : (var29 >= 1.0 ? 0.0 : 1.0 - var16 * var16 * (3.0 - 2.0 * var16));
                    var1[var21] =
                        (float)((double)(var7 == 0 ? 64.0F : 48.0F) * Math::exp(-0.5 * var30 * var30) * var18);
                }
            } else {
                double var6 = 0.0;
                if (var5 >= (1793276841 ^ 1793276713) && var5 < (900382535 ^ 900381880)) {
                    double var8 = ((double)(var5 - (1754525252 ^ 1754525380)) / 894.0 * 2.0 - 1.0) * 24.0;
                    double var10 = Math::abs(var8) / 24.0;
                    double var12 = (var10 - 0.55) / 0.45;
                    double var14 =
                        var10 < 0.55 ? 1.0 : (var10 >= 0.9999 ? 0.0 : 1.0 - var12 * var12 * (3.0 - 2.0 * var12));
                    var6 = 64.0 * Math::exp(-(Math::sqrt(var8 * var8 + 4.0) - 2.0) / 10.0) * var14;
                }

                for (int var24 = 0; var24 < (1230106945 ^ 1230106689); var24++) {
                    var1[var24] = (float)var6;
                }
            }

            int var22 = var5 * (922954325 ^ 922954581);

            for (int var23 = 0; var23 < (1050486278 ^ 1050486534); var23++) {
                float var26 = var1[var23];
                float var9 = var26 < 1.0F ? var26 : 1.0F;
                float var28 = var26 + (var2.nextFloat() - var2.nextFloat()) * var9 + 0.5F;
                int var11 = var28 < 0.0F ? 0 : (var28 >= 255.0F ? 1449739385 ^ 1449739398 : (int)var28);
                var0[var22 + var23] = 429066253 ^ -426571763 | var11 << 16 | var11 << 8 | var11;
            }
        }

        return var0;
    } catch (...) {
        throw;
    }
}

int b(float var0, double var1, float var3, float var4, float var5, float var6, float var7) {

    try {
        if (az < 0.0 || az > var1 + 60.0) {
            az = var1 + 6.0 + 10.0 * ay.nextDouble();
        }

        if (var1 >= az) {
            if (var1 - az > 1.0) {
                az = var1 + 4.0 + 8.0 * ay.nextDouble();
            } else {
                ba = var1;
                bb = 0.15F + 0.7F * ay.nextFloat();
                bc = ay.nextBoolean() ? 1.0F : -1.0F;
                az = var1 + 12.0 + 26.0 * ay.nextDouble();
                bd++;
            }
        }

        float var8 = 0.0F;
        float var9 = 0.5F;
        if (ba >= 0.0) {
            float var10 = (float)(var1 - ba);
            if (var10 >= 0.0F && var10 < 1.9F) {
                float var11 = var10 / 1.9F;
                float var12 = ae[(int)(var11 * (float)Math::PI * 10430.378F) & (-1234639998 ^ -1234690947)];
                var8 = var12 * var12;
                var9 = bb + bc * 0.12F * var11;
            } else {
                ba = -1.0;
            }
        }

        float var30 = (float)(std::fmod(var1, 6283.185307179586));
        float var31 = 0.95F + 0.05F * ae[(int)(var30 * 0.4F * 10430.378F) & (163343717 ^ 163353242)];
        double var32 = Math::acos(var7 < -1.0F ? -1.0 : (var7 > 1.0F ? 1.0 : (double)var7));
        Array<float> var14 = ao;
        Array<float> var15 = ap;
        int var16 = 0;
        int var17 = c(0, false, var1);
        float var18 = 1.0F / (float)(var17 - 1);

        for (int var19 = 0; var19 < var17; var19++) {
            float var20 = (float)var19 * var18;
            float var21 = var20 * 10.0F;
            float var22 = (1.0F - var20) * 10.0F;
            var21 = var21 > 1.0F ? 1.0F : var21;
            var22 = var22 > 1.0F ? 1.0F : var22;
            float var23 = var21 * var21 * (3.0F - 2.0F * var21) * var22 * var22 * (3.0F - 2.0F * var22);
            float var24 =
                ae[(int)((43.9823F * var20 - 1.1F * var30 + 0.3F) * 10430.378F) & (-694806132 ^ -694753677)] *
                (0.6F +
                 0.4F * ae[(int)((14.451326F * var20 + 0.37F * var30) * 10430.378F) & (-1501332516 ^ -1501330397)]);
            float var25 = ae[(int)((119.38052F * var20 + 2.3F * var30) * 10430.378F) & (-1107134340 ^ -1107130493)];
            float var26 = (var20 - var9) * 16.666668F;
            float var27 = var26 * var26;
            float var28 = var27 > 9.0F
                              ? 0.0F
                              : var8 / (1.0F + var27 + 0.5F * var27 * var27 + 0.16666667F * var27 * var27 * var27);
            var14[var19] = var3 * var23 * var31 * (0.9F + 0.08F * var24 + 0.04F * var25);
            var15[var19] = var3 * var23 * 1.3F * var28;
        }

        var16 = d(var17, var16, var0, 2.8F, 3.2F, 1, 6, var4, var5, var6, var32);
        var16 = d(var17, var16, var0, 1.0F, 0.9F, 0, 3, var4, var5, var6, var32);
        bool var45 = var8 > 0.0F;
        if (var45) {
            for (int var46 = 0; var46 < var17; var46++) {
                float var52 = var14[var46];
                var14[var46] = var15[var46];
                var15[var46] = var52;
            }

            var16 = d(var17, var16, var0, 2.8F, 3.2F, 1, 6, var4, var5, var6, var32);
            var16 = d(var17, var16, var0, 1.0F, 0.9F, 0, 3, var4, var5, var6, var32);

            for (int var47 = 0; var47 < var17; var47++) {
                var14[var47] += var15[var47];
            }
        }

        var16 = d(var17, var16, var0, 0.42F, 0.4F, 0, 0, var4, var5, var6, var32);
        var17 = c(0, true, var1);
        var18 = 1.0F / (float)(var17 - 1);

        for (int var48 = 0; var48 < var17; var48++) {
            float var53 = (float)var48 * var18;
            float var56 = var53 * 8.0F;
            float var59 = (1.0F - var53) * 8.0F;
            var56 = var56 > 1.0F ? 1.0F : var56;
            var59 = var59 > 1.0F ? 1.0F : var59;
            float var63 = var56 * var56 * (3.0F - 2.0F * var56) * var59 * var59 * (3.0F - 2.0F * var59);
            float var66 = (var53 - var9) * 12.5F;
            float var68 = var66 * var66;
            float var70 = var68 > 9.0F
                              ? 0.0F
                              : var8 / (1.0F + var68 + 0.5F * var68 * var68 + 0.16666667F * var68 * var68 * var68);
            var14[var48] = var3 * var63 * var31;
            var15[var48] = var3 * var63 * var70;
        }

        var16 = e(var17, var16, var0, 24.0F, 0.71F, var4, var5, var6, var32);
        if (var45) {
            for (int var49 = 0; var49 < var17; var49++) {
                var14[var49] = var15[var49];
            }

            var16 = e(var17, var16, var0, 24.0F, 0.29F, var4, var5, var6, var32);
        }

        var17 = c(1, false, var1);
        var18 = 1.0F / (float)(var17 - 1);
        float var50 =
            var3 * 0.36F * (0.82F + 0.18F * ae[(int)((0.07F * var30 + 1.0F) * 10430.378F) & (1373536121 ^ 1373536390)]);

        for (int var54 = 0; var54 < var17; var54++) {
            float var58 = (float)var54 * var18;
            float var61 = var58 * 7.0F;
            float var64 = (1.0F - var58) * 7.0F;
            var61 = var61 > 1.0F ? 1.0F : var61;
            var64 = var64 > 1.0F ? 1.0F : var64;
            float var67 = var61 * var61 * (3.0F - 2.0F * var61) * var64 * var64 * (3.0F - 2.0F * var64);
            float var69 = ae[(int)((31.415928F * var58 + 0.9F * var30 + 2.0F) * 10430.378F) & (284421184 ^ 284365759)];
            var14[var54] = var50 * var67 * (0.92F + 0.08F * var69);
        }

        var16 = e(var17, var16, var0, 14.400001F, 0.13F, var4, var5, var6, var32);
        var16 = d(var17, var16, var0, 1.9599999F, 2.24F, 1, 6, var4, var5, var6, var32);
        var16 = d(var17, var16, var0, 0.75F, 0.67499995F, 0, 3, var4, var5, var6, var32);
        return d(var17, var16, var0, 0.336F, 0.32000002F, 0, 0, var4, var5, var6, var32);
    } catch (...) {
        throw;
    }
}

int c(int var0, bool var1, double var2) {

    try {
        int var4 = var0 * 4;
        float var5 = v[var4] * (float)(Math::PI / 180.0);
        float var6 = v[var4 + 1] * (float)(Math::PI / 180.0);
        float var7 = v[var4 + 2] * (float)(Math::PI / 180.0);
        int var8 = var1 ? 75 : (int)v[var4 + 3];
        Array<float> var9 = au;
        Array<float> var10 = av;
        Array<float> var11 = aw;
        Array<float> var12 = w;

        for (int var13 = 0; var13 < 8; var13++) {
            int var14 = var0 * 32 + var13 * 4;
            float var15 = var1 ? x[var13] : 1.0F;
            double var16 = var13 < 4 ? 1.0 : 1.0 + 0.15 * Math::sin(0.011 * var2 + (double)var13 * 1.7);
            var9[var13] = (float)((double)(var12[var14] * (float)(Math::PI / 180.0) * var15) * var16);
            var10[var13] = var12[var14 + 1] * (float)(Math::PI * 2);
            var11[var13] = (float)(std::fmod((double)var12[var14 + 2] * var2 + (double)var12[var14 + 3], Math::PI * 2));
        }

        Array<float> var40 = ah;
        Array<float> var41 = ai;
        Array<float> var42 = aj;
        Array<float> var43 = ak;
        Array<float> var17 = al;
        Array<float> var18 = am;
        Array<float> var19 = an;
        Array<float> var20 = ae;
        float var21 = -0.05235988F;
        int var22 = var8 + 1;
        float var23 = 1.0F / (float)var8;

        for (int var24 = 0; var24 < var22; var24++) {
            float var25 = (float)var24 * var23;
            float var26 =
                var5 + var6 * var25 +
                var9[0] * var20[(int)((var10[0] * var25 + var11[0]) * 10430.378F) & (-1022926987 ^ -1022910326)] +
                var9[1] * var20[(int)((var10[1] * var25 + var11[1]) * 10430.378F) & (540547401 ^ 540599990)] +
                var9[2] * var20[(int)((var10[2] * var25 + var11[2]) * 10430.378F) & (-526875887 ^ -526877458)] +
                var9[3] * var20[(int)((var10[3] * var25 + var11[3]) * 10430.378F) & (-711750023 ^ -711757434)];
            float var27 =
                var7 +
                var9[4] * var20[(int)((var10[4] * var25 + var11[4]) * 10430.378F) & (-1793434897 ^ -1793415920)] +
                var9[5] * var20[(int)((var10[5] * var25 + var11[5]) * 10430.378F) & (1311965637 ^ 1312030266)] +
                var9[6] * var20[(int)((var10[6] * var25 + var11[6]) * 10430.378F) & (-394558887 ^ -394560090)] +
                var9[7] * var20[(int)((var10[7] * var25 + var11[7]) * 10430.378F) & (-1063114433 ^ -1063070016)];
            float var28 = var25 * 7.142857F;
            float var29 = (1.0F - var25) * 7.142857F;
            var28 = var28 > 1.0F ? 1.0F : var28;
            var29 = var29 > 1.0F ? 1.0F : var29;
            float var30 = var28 * var28 * (3.0F - 2.0F * var28) * var29 * var29 * (3.0F - 2.0F * var29);
            float var31 = var21 + (var27 - var21) * var30;
            float var32 = var20[(int)(var31 * 10430.378F + 16384.0F) & (-1362106891 ^ -1362159094)];
            float var33 = var20[(int)(var31 * 10430.378F) & (-1117616858 ^ -1117619495)];
            float var34 = var20[(int)(var26 * 10430.378F) & (1450305623 ^ 1450252200)];
            float var35 = var20[(int)(var26 * 10430.378F + 16384.0F) & (-1624132504 ^ -1624159337)];
            var40[var24] = var32 * var34;
            var41[var24] = var33;
            var42[var24] = -var32 * var35;
        }

        float var44 = 0.0F;

        for (int var45 = 0; var45 < var22; var45++) {
            int var46 = var45 > 0 ? var45 - 1 : 0;
            int var47 = var45 < var22 - 1 ? var45 + 1 : var22 - 1;
            float var49 = var40[var47] - var40[var46];
            float var51 = var41[var47] - var41[var46];
            float var52 = var42[var47] - var42[var46];
            float var53 = var41[var45] * var52 - var42[var45] * var51;
            float var54 = var42[var45] * var49 - var40[var45] * var52;
            float var55 = var40[var45] * var51 - var41[var45] * var49;
            float var56 = (float)Math::sqrt((double)(var53 * var53 + var54 * var54 + var55 * var55));
            float var57 = var56 > 1.0E-9F ? 1.0F / var56 : 0.0F;
            var43[var45] = var53 * var57;
            var17[var45] = var54 * var57;
            var18[var45] = var55 * var57;
            if (var45 > 0) {
                float var36 = var40[var45] - var40[var45 - 1];
                float var37 = var41[var45] - var41[var45 - 1];
                float var38 = var42[var45] - var42[var45 - 1];
                var44 += (float)Math::sqrt((double)(var36 * var36 + var37 * var37 + var38 * var38)) *
                         (float)(180.0 / Math::PI);
            }

            var19[var45] = var44;
        }

        return var22;
    } catch (...) {
        throw;
    }
}

int d(int var0, int var1, float var2, float var3, float var4, int var5, int var6, float var7, float var8, float var9,
      double var10) {

    try {
        float var12 = var3 * (var5 == 0 ? 3.0F : 2.6F) * (float)(Math::PI / 180.0);
        float var13 = var2 * var12 * (1.0F + var12 * var12 * 0.33333334F);
        double var14 = var10 + (double)var12 * 1.42 + 0.02;
        float var16 = var14 >= Math::PI ? -2.0F : (float)Math::cos(var14);
        float var17 = ((float)(var5 * (-157695382 ^ -157695254)) + 0.5F) / 256.0F;
        float var18 = ((float)(var5 * (-1857026928 ^ -1857027056) + (844125580 ^ 844125452)) - 0.5F) / 256.0F;
        float var19 = 4.8828125E-4F;
        float var20 = 0.12451172F;
        float var21 = u[var6];
        float var22 = u[var6 + 1];
        float var23 = u[var6 + 2];
        bool var24 = var6 != 0;
        Array<float> var25 = ah;
        Array<float> var26 = ai;
        Array<float> var27 = aj;
        Array<float> var28 = ak;
        Array<float> var29 = al;
        Array<float> var30 = am;
        Array<float> var31 = an;
        Array<float> var32 = ao;
        Array<float> var33 = VB;
        float var34 = var31[var0 - 1];
        int var35 = var33.size() / 6 - 4;
        int var36 = 0;

        for (float var37 = var4 * 0.5F; var37 < var34 && var1 <= var35; var37 += var4) {
            while (var36 < var0 - 2 && var31[var36 + 1] < var37) {
                var36++;
            }

            float var38 = var31[var36 + 1] - var31[var36];
            float var39 = var38 > 1.0E-6F ? (var37 - var31[var36]) / var38 : 0.0F;
            var39 = var39 < 0.0F ? 0.0F : (var39 > 1.0F ? 1.0F : var39);
            float var40 = var25[var36] + (var25[var36 + 1] - var25[var36]) * var39;
            float var41 = var26[var36] + (var26[var36 + 1] - var26[var36]) * var39;
            float var42 = var27[var36] + (var27[var36 + 1] - var27[var36]) * var39;
            if (!(var40 * var7 + var41 * var8 + var42 * var9 < var16)) {
                float var43 = var32[var36] + (var32[var36 + 1] - var32[var36]) * var39;
                int var44 = (int)(var21 * var43 + 0.5F);
                int var45 = (int)(var22 * var43 + 0.5F);
                int var46 = (int)(var23 * var43 + 0.5F);
                if ((var44 | var45 | var46) > 0) {
                    float var47 = 1.0F / (float)Math::sqrt((double)(var40 * var40 + var41 * var41 + var42 * var42));
                    var40 *= var47;
                    var41 *= var47;
                    var42 *= var47;
                    float var48 = var28[var36] + (var28[var36 + 1] - var28[var36]) * var39;
                    float var49 = var29[var36] + (var29[var36 + 1] - var29[var36]) * var39;
                    float var50 = var30[var36] + (var30[var36 + 1] - var30[var36]) * var39;
                    float var51 = var48 * var48 + var49 * var49 + var50 * var50;
                    if (!(var51 < 1.0E-8F)) {
                        var51 = var13 / (float)Math::sqrt((double)var51);
                        var48 *= var51;
                        var49 *= var51;
                        var50 *= var51;
                        float var52 = var49 * var42 - var50 * var41;
                        float var53 = var50 * var40 - var48 * var42;
                        float var54 = var48 * var41 - var49 * var40;
                        int var55 = var45 > var46 ? var45 : var46;
                        if (var24 && var55 > (1680493106 ^ 1680493261)) {
                            float var56 = 255.0F / (float)var55;
                            var44 = (int)((float)var44 * var56);
                            var45 = (int)((float)var45 * var56);
                            var46 = (int)((float)var46 * var56);
                        }

                        var44 = var44 > 127 ? 127 : var44;
                        var45 = var45 > (-626208640 ^ -626208641) ? -29936762 ^ -29936775 : var45;
                        var46 = var46 > (-627143863 ^ -627143754) ? -293150025 ^ -293150136 : var46;
                        float var73 =
                            Float::intBitsToFloat(-1678072659 ^ 1694147757 | var44 << 16 | var45 << 8 | var46);
                        float var57 = var40 * var2;
                        float var58 = var41 * var2;
                        float var59 = var42 * var2;
                        int var60 = var1 * 6;
                        var33[var60] = var57 - var48 - var52;
                        var33[var60 + 1] = var58 - var49 - var53;
                        var33[var60 + 2] = var59 - var50 - var54;
                        var33[var60 + 3] = var17;
                        var33[var60 + 4] = var19;
                        var33[var60 + 5] = var73;
                        var33[var60 + 6] = var57 - var48 + var52;
                        var33[var60 + 7] = var58 - var49 + var53;
                        var33[var60 + 8] = var59 - var50 + var54;
                        var33[var60 + 9] = var17;
                        var33[var60 + 10] = var20;
                        var33[var60 + 11] = var73;
                        var33[var60 + 12] = var57 + var48 + var52;
                        var33[var60 + 13] = var58 + var49 + var53;
                        var33[var60 + 14] = var59 + var50 + var54;
                        var33[var60 + 15] = var18;
                        var33[var60 + 16] = var20;
                        var33[var60 + 17] = var73;
                        var33[var60 + 18] = var57 + var48 - var52;
                        var33[var60 + 19] = var58 + var49 - var53;
                        var33[var60 + 20] = var59 + var50 - var54;
                        var33[var60 + 21] = var18;
                        var33[var60 + 22] = var19;
                        var33[var60 + 23] = var73;
                        var1 += 4;
                    }
                }
            }
        }

        return var1;
    } catch (...) {
        throw;
    }
}

int e(int var0, int var1, float var2, float var3, float var4, float var5, float var6, float var7, double var8) {

    try {
        float var10 = var3 * (float)(Math::PI / 180.0);
        float var11 = var2 * var10 * (1.0F + var10 * var10 * 0.33333334F);
        float var12 = u[9];
        float var13 = u[10];
        float var14 = u[11];
        double var15 = var8 + (double)var10 + 0.08;
        float var17 = var15 >= Math::PI ? -2.0F : (float)Math::cos(var15);
        float var18 = 0.12548828F;
        float var19 = 0.5620117F;
        float var20 = 0.99853516F;
        float var21 = 894.0F / (2.0F * var3 * 256.0F);
        Array<float> var22 = ah;
        Array<float> var23 = ai;
        Array<float> var24 = aj;
        Array<float> var25 = an;
        Array<float> var26 = ao;
        Array<float> var27 = aq;
        Array<float> var28 = ar;
        Array<float> var29 = as;
        Array<float> var30 = at;
        Array<float> var31 = VB;

        for (int var32 = 0; var32 < var0; var32++) {
            float var33 = -var22[var32] * var23[var32];
            float var34 = 1.0F - var23[var32] * var23[var32];
            float var35 = -var24[var32] * var23[var32];
            float var36 = var33 * var33 + var34 * var34 + var35 * var35;
            var36 = var36 > 1.0E-8F ? var11 / (float)Math::sqrt((double)var36) : 0.0F;
            var27[var32] = var33 * var36;
            var28[var32] = var34 * var36;
            var29[var32] = var35 * var36;
            float var37 = var26[var32];
            int var38 = (int)(var12 * var37 + 0.5F);
            int var39 = (int)(var13 * var37 + 0.5F);
            int var40 = (int)(var14 * var37 + 0.5F);
            int var41 = var39 > var40 ? var39 : var40;
            if (var41 > (-630903391 ^ -630903458)) {
                float var42 = 255.0F / (float)var41;
                var38 = (int)((float)var38 * var42);
                var39 = (int)((float)var39 * var42);
                var40 = (int)((float)var40 * var42);
            }

            var38 = var38 < 0 ? 0 : (var38 > 127 ? 127 : var38);
            var39 = var39 < 0 ? 0 : (var39 > (-1981995285 ^ -1981995500) ? 438247691 ^ 438247924 : var39);
            var40 = var40 < 0 ? 0 : (var40 > (201567111 ^ 201567096) ? -461653135 ^ -461653106 : var40);
            var30[var32] = Float::intBitsToFloat(1349598106 ^ -1351533670 | var38 << 16 | var39 << 8 | var40);
        }

        int var53 = var31.size() / 6 - 8;

        for (int var54 = 0; var54 < var0 - 1 && var1 <= var53; var54++) {
            int var55 = var54 + 1;
            if (!(var22[var54] * var5 + var23[var54] * var6 + var24[var54] * var7 < var17) ||
                !(var22[var55] * var5 + var23[var55] * var6 + var24[var55] * var7 < var17)) {
                float var56 = var30[var54];
                float var58 = var30[var55];
                if ((Float::floatToRawIntBits(var56) & (-1157565506 ^ -1140913087)) != 0 ||
                    (Float::floatToRawIntBits(var58) & (-2137399560 ^ -2140790521)) != 0) {
                    float var59 = var22[var54] * var2;
                    float var61 = var23[var54] * var2;
                    float var63 = var24[var54] * var2;
                    float var65 = var22[var55] * var2;
                    float var66 = var23[var55] * var2;
                    float var67 = var24[var55] * var2;
                    float var43 = var27[var54];
                    float var44 = var28[var54];
                    float var45 = var29[var54];
                    float var46 = var27[var55];
                    float var47 = var28[var55];
                    float var48 = var29[var55];
                    float var49 = var25[var54] * var21 + var4;
                    float var50 = var25[var55] * var21 + var4;
                    int var51 = var1 * 6;
                    var31[var51] = var59 - var43;
                    var31[var51 + 1] = var61 - var44;
                    var31[var51 + 2] = var63 - var45;
                    var31[var51 + 3] = var49;
                    var31[var51 + 4] = var18;
                    var31[var51 + 5] = var56;
                    var31[var51 + 6] = var59;
                    var31[var51 + 7] = var61;
                    var31[var51 + 8] = var63;
                    var31[var51 + 9] = var49;
                    var31[var51 + 10] = var19;
                    var31[var51 + 11] = var56;
                    var31[var51 + 12] = var65;
                    var31[var51 + 13] = var66;
                    var31[var51 + 14] = var67;
                    var31[var51 + 15] = var50;
                    var31[var51 + 16] = var19;
                    var31[var51 + 17] = var58;
                    var31[var51 + 18] = var65 - var46;
                    var31[var51 + 19] = var66 - var47;
                    var31[var51 + 20] = var67 - var48;
                    var31[var51 + 21] = var50;
                    var31[var51 + 22] = var18;
                    var31[var51 + 23] = var58;
                    var31[var51 + 24] = var59;
                    var31[var51 + 25] = var61;
                    var31[var51 + 26] = var63;
                    var31[var51 + 27] = var49;
                    var31[var51 + 28] = var19;
                    var31[var51 + 29] = var56;
                    var31[var51 + 30] = var59 + var43;
                    var31[var51 + 31] = var61 + var44;
                    var31[var51 + 32] = var63 + var45;
                    var31[var51 + 33] = var49;
                    var31[var51 + 34] = var20;
                    var31[var51 + 35] = var56;
                    var31[var51 + 36] = var65 + var46;
                    var31[var51 + 37] = var66 + var47;
                    var31[var51 + 38] = var67 + var48;
                    var31[var51 + 39] = var50;
                    var31[var51 + 40] = var20;
                    var31[var51 + 41] = var58;
                    var31[var51 + 42] = var65;
                    var31[var51 + 43] = var66;
                    var31[var51 + 44] = var67;
                    var31[var51 + 45] = var50;
                    var31[var51 + 46] = var19;
                    var31[var51 + 47] = var58;
                    var1 += 8;
                }
            }
        }

        return var1;
    } catch (...) {
        throw;
    }
}

const bool initialized = []() {
    for (int var0 = 0; var0 < 65536; var0++) {
        ae[var0] = (float)Math::sin((double)var0 * Math::PI * 2.0 / 65536.0);
    }
    return true;
}();
} // namespace vanta::modules::bloom::ribbon
