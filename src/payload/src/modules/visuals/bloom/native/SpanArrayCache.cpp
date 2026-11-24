// Ported from Bloom 2.2; numerical source preserved. Native C++ only.
#include "NativeImages.h"
namespace vanta::modules::bloom::span {

int F = 1024;
int G = 2;
int C = 1028;
int CW = 3084;
int CH = 2056;
int SW = 1024;
int SH = 1024;
int NSTAR = 2400;
int NUNI = 1320;
int SST = 9;
Array<float> FACE = Array<float>{
    1.0F, 0.0F, 0.0F, 0.0F,  0.0F, 1.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F,  0.0F,  0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,  0.0F,  -1.0F, 1.0F, 0.0F, 0.0F,  0.0F, 1.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 1.0F,  0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,  -1.0F, 0.0F,  1.0F, 0.0F, 0.0F,  0.0F, 0.0F, -1.0F};
double NX;
double NY;
double NZ;
double E1X;
double E1Y;
double E1Z;
double E2X;
double E2Y;
double E2Z;
Array<double> NEB = Array<double>{-0.55, 0.42, -0.72, 0.26, 1.0,  0.2,  0.42, 0.25, 0.55, 1.0,  4.5, 11.0, 0.95,
                                  0.62,  0.55, 0.56,  0.22, 0.12, 0.85, 0.8,  0.55, 0.3,  0.95, 5.5, 27.0, 0.8,
                                  0.15,  0.3,  0.94,  0.17, 1.0,  0.55, 0.2,  0.7,  0.3,  0.85, 6.5, 43.0, 0.7};
int NB = 13;
Array<double> t = Array<double>(3);
double u = Math::sin(0.7);
double v;
double w;
double x;
double y;
double z;
double aa;
double ab = 1.28;
double ac = 2.28;
Array<float> ad = Array<float>(262144);
Array<float> ae = Array<float>(4097);
Array<double> af = Array<double>{0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};
int ag = 0;
int ah = 1;
int ai = 2;
int aj = 3;
int ak = 4;
int al = 5;
int am = 6;
int an = 7;
int ao = 8;
int ap = 9;
int aq = 10;
int ar = 11;
int as = 12;
int at = 13;
int au = 14;
int av = 15;
int aw = 16;
int ax = 17;
int ay = 18;
int az = 19;
int ba = 20;
int bb = 21;
bool bc = false;

Array<int> a(int var0) {
    Array<int> pixels = Array<int>(6340704);
    bc = false;
    b(pixels, 0, 1);
    return pixels;
}

void b(Array<int> var0, int var1, int var2) {

    try {
        Array<double> var3 = Array<double>(1028);
        Array<double> var4 = Array<double>(1028);
        Array<double> var5 = Array<double>(1028);
        Array<double> var6 = Array<double>(1028);
        Array<std::int8_t> var7 = Array<std::int8_t>(1028);
        Array<int> var8 = Array<int>(4);
        Array<int> var9 = Array<int>(4);
        Array<Array<double>> var10 = matrix<double>(21, 1028);
        Array<double> var11 = var10[15];
        Array<double> var12 = var10[16];
        Array<double> var13 = var10[3];
        Array<double> var14 = var10[4];

        for (int var15 = 0; var15 < 6 && !bc; var15++) {
            int var16 = var1;

            while (var16 < (-1789353489 ^ -1789352469)) {
                c(var15, var16, var3, var4, var5, var6, var7, var8, var9);
                d(var3, var4, var5, var7, 0, 0, 1303533190 ^ 1303532162, 1.3, 3.1, 7.7, 1.9, nullptr, nullptr, 0.0, 3,
                  0.0, var10[0]);
                d(var3, var4, var5, var7, 0, 0, 740830516 ^ 740829488, 2.1, 13.1, 2.7, 5.3, nullptr, nullptr, 0.0, 2,
                  0.0, var10[1]);
                d(var3, var4, var5, var7, 0, 0, -909466815 ^ -909465787, 0.9, 50.0, 8.0, 21.0, nullptr, nullptr, 0.0, 3,
                  0.0, var10[2]);
                int var17 = var8[0];
                int var18 = var9[0];
                if (var17 < var18) {
                    d(var3, var4, var5, var7, 1, var17, var18, 3.0, 5.0, 9.0, 1.0, nullptr, nullptr, 0.0, 2, 0.5,
                      var13);
                    d(var3, var4, var5, var7, 1, var17, var18, 3.0, 21.0, 2.0, 7.0, nullptr, nullptr, 0.0, 2, 0.5,
                      var14);
                    d(var3, var4, var5, var7, 1, var17, var18, 2.4, 31.0, 3.0, 17.0, nullptr, nullptr, 0.0, 3, 0.5,
                      var10[5]);
                    d(var3, var4, var5, var7, 1, var17, var18, 5.0, 40.0, 11.0, 23.0, var13, var14, 1.4, 5, 0.0,
                      var10[6]);
                    d(var3, var4, var5, var7, 1, var17, var18, 13.0, 3.0, 61.0, 5.0, var14, var13, 0.8, 4, 0.0,
                      var10[7]);
                    d(var3, var4, var5, var7, 1, var17, var18, 7.0, 70.0, 30.0, 90.0, var13, var14, 2.2, 5, 0.0,
                      var10[8]);
                    d(var3, var4, var5, var7, 1, var17, var18, 3.6, 120.0, 7.0, 55.0, var14, var13, 1.6, 4, 0.0,
                      var10[9]);
                    d(var3, var4, var5, var7, 1, var17, var18, 20.0, 101.0, 7.0, 44.0, nullptr, nullptr, 0.0, 3, 0.0,
                      var10[10]);
                }

                e(var3, var4, var5, var7, var10, var17, var18);

                for (int var19 = 0; var19 < 3; var19++) {
                    var17 = var8[var19 + 1];
                    var18 = var9[var19 + 1];
                    if (var17 < var18) {
                        int var20 = 2 << var19;
                        int var21 = var19 * 13;
                        double var22 = NEB[var21 + 10];
                        double var24 = NEB[var21 + 11];
                        d(var3, var4, var5, var7, var20, var17, var18, var22 * 0.55, var24, var24 * 0.7, var24 * 1.3,
                          nullptr, nullptr, 0.0, 3, 0.5, var11);
                        d(var3, var4, var5, var7, var20, var17, var18, var22 * 0.55, var24 * 1.9, var24 * 0.3,
                          var24 * 0.5, nullptr, nullptr, 0.0, 3, 0.5, var12);
                        d(var3, var4, var5, var7, var20, var17, var18, var22, var24, var24, var24, var11, var12, 3.0, 5,
                          0.0, var10[17]);
                        d(var3, var4, var5, var7, var20, var17, var18, var22 * 2.3, var24 * 2.0, 0.0, var24, var12,
                          var11, 3.5, 5, 0.0, var10[18]);
                        d(var3, var4, var5, var7, var20, var17, var18, var22 * 1.7, 200.0, 50.0, var24, nullptr,
                          nullptr, 0.0, 4, 0.0, var10[19]);
                        d(var3, var4, var5, var7, var20, var17, var18, var22 * 0.8, 300.0, var24, 0.0, nullptr, nullptr,
                          0.0, 3, 0.0, var10[20]);
                        f(var19, var3, var4, var5, var7, var10, var17, var18);
                    }
                }

                g(var0, var15, var16, var3, var4, var5, var6, var10);
                var16 += var2;
            }
        }
    } catch (...) {
        bc = true;
    }
}

void c(int var0, int var1, Array<double> var2, Array<double> var3, Array<double> var4, Array<double> var5,
       Array<std::int8_t> var6, Array<int> var7, Array<int> var8) {

    try {
        int var9 = 9 * var0;
        double var10 = (double)FACE[var9];
        double var12 = (double)FACE[var9 + 1];
        double var14 = (double)FACE[var9 + 2];
        double var16 = (double)FACE[var9 + 3];
        double var18 = (double)FACE[var9 + 4];
        double var20 = (double)FACE[var9 + 5];
        double var22 = (double)FACE[var9 + 6];
        double var24 = (double)FACE[var9 + 7];
        double var26 = (double)FACE[var9 + 8];
        double var28 = ((double)(var1 - 2) + 0.5) * 2.0 / 1024.0 - 1.0;
        double var30 = NEB[0];
        double var32 = NEB[1];
        double var34 = NEB[2];
        double var36 = NEB[13];
        double var38 = NEB[14];
        double var40 = NEB[15];
        double var42 = NEB[26];
        double var44 = NEB[27];
        double var46 = NEB[28];
        double var48 = t[0];
        double var50 = t[1];
        double var52 = t[2];
        double var54 = u;
        double var56 = NX;
        double var58 = NY;
        double var60 = NZ;

        for (int var62 = 0; var62 < 4; var62++) {
            var7[var62] = 1028;
            var8[var62] = 0;
        }

        for (int var79 = 0; var79 < (1300051931 ^ 1300050911); var79++) {
            double var63 = ((double)(var79 - 2) + 0.5) * 2.0 / 1024.0 - 1.0;
            double var65 = var10 + var63 * var16 - var28 * var22;
            double var67 = var12 + var63 * var18 - var28 * var24;
            double var69 = var14 + var63 * var20 - var28 * var26;
            double var71 = var65 * var65 + var67 * var67 + var69 * var69;
            double var73 = Math::sqrt(var71);
            var65 /= var73;
            var67 /= var73;
            var69 /= var73;
            var2[var79] = var65;
            var3[var79] = var67;
            var4[var79] = var69;
            var5[var79] = 0.001953125 / var71;
            int var75 = 0;
            double var76 = var65 * var56 + var67 * var58 + var69 * var60;
            if (var76 < var54 && var76 > -var54) {
                var75 = 1;
                if (var79 < var7[0]) {
                    var7[0] = var79;
                }

                var8[0] = var79 + 1;
            }

            if (var65 * var30 + var67 * var32 + var69 * var34 >= var48) {
                var75 |= 2;
                if (var79 < var7[1]) {
                    var7[1] = var79;
                }

                var8[1] = var79 + 1;
            }

            if (var65 * var36 + var67 * var38 + var69 * var40 >= var50) {
                var75 |= 4;
                if (var79 < var7[2]) {
                    var7[2] = var79;
                }

                var8[2] = var79 + 1;
            }

            if (var65 * var42 + var67 * var44 + var69 * var46 >= var52) {
                var75 |= 8;
                if (var79 < var7[3]) {
                    var7[3] = var79;
                }

                var8[3] = var79 + 1;
            }

            var6[var79] = (int)var75;
        }
    } catch (...) {
        throw;
    }
}

void d(Array<double> var0, Array<double> var1, Array<double> var2, Array<std::int8_t> var3, int var4, int var5,
       int var6, double var7, double var9, double var11, double var13, Array<double> var15, Array<double> var16,
       double var17, int var19, double var20, Array<double> var22) {

    try {
        Array<float> var23 = ad;
        double var24 = 1.0 / (1.0 - Math::pow(0.5, (double)var19));
        bool var26 = var15 != nullptr;

        for (int var27 = var5; var27 < var6; var27++) {
            if (var4 == 0 || (var3[var27] & var4) != 0) {
                double var28 = var0[var27] * var7 + var9;
                double var30 = var1[var27] * var7 + var11;
                double var32 = var2[var27] * var7 + var13;
                if (var26) {
                    var28 += var15[var27] * var17;
                    var30 += var16[var27] * var17;
                }

                double var34 = 0.0;
                double var36 = 0.5;

                for (int var38 = 0; var38 < var19; var38++) {
                    int var39 = (int)var28;
                    int var40 = (int)var30;
                    int var41 = (int)var32;
                    if (var28 < (double)var39) {
                        var39--;
                    }

                    if (var30 < (double)var40) {
                        var40--;
                    }

                    if (var32 < (double)var41) {
                        var41--;
                    }

                    double var42 = var28 - (double)var39;
                    double var44 = var30 - (double)var40;
                    double var46 = var32 - (double)var41;
                    var42 = var42 * var42 * (3.0 - 2.0 * var42);
                    var44 = var44 * var44 * (3.0 - 2.0 * var44);
                    var46 = var46 * var46 * (3.0 - 2.0 * var46);
                    int var48 = var39 & 63;
                    int var49 = var39 + 1 & 63;
                    int var50 = (var40 & 63) << 6;
                    int var51 = (var40 + 1 & 63) << 6;
                    int var52 = (var41 & 63) << 12;
                    int var53 = (var41 + 1 & 63) << 12;
                    double var54 = (double)var23[var52 | var50 | var48];
                    double var56 = (double)var23[var52 | var51 | var48];
                    double var58 = (double)var23[var53 | var50 | var48];
                    double var60 = (double)var23[var53 | var51 | var48];
                    var54 += ((double)var23[var52 | var50 | var49] - var54) * var42;
                    var56 += ((double)var23[var52 | var51 | var49] - var56) * var42;
                    var58 += ((double)var23[var53 | var50 | var49] - var58) * var42;
                    var60 += ((double)var23[var53 | var51 | var49] - var60) * var42;
                    var54 += (var56 - var54) * var44;
                    var58 += (var60 - var58) * var44;
                    var34 += var36 * (var54 + (var58 - var54) * var46);
                    double var62 = 1.62 * var28 + 1.21 * var32 + 17.3;
                    double var64 = 2.03 * var30 + 5.1;
                    var32 = -1.21 * var28 + 1.62 * var32 + 9.7;
                    var28 = var62;
                    var30 = var64;
                    var36 *= 0.5;
                }

                var22[var27] = var34 * var24 - var20;
            }
        }
    } catch (...) {
        throw;
    }
}

void e(Array<double> var0, Array<double> var1, Array<double> var2, Array<std::int8_t> var3, Array<Array<double>> var4,
       int var5, int var6) {

    try {
        Array<double> var7 = var4[0];
        Array<double> var8 = var4[1];
        Array<double> var9 = var4[2];
        Array<double> var10 = var4[11];
        Array<double> var11 = var4[12];
        Array<double> var12 = var4[13];
        Array<double> var13 = var4[14];

        for (int var14 = 0; var14 < (218022743 ^ 218023763); var14++) {
            double var15 = var9[var14] * var9[var14];
            double var17 = var7[var14];
            double var19 = var8[var14];
            var10[var14] = 0.002 + 0.0028 * var17 + 0.0014 * var19 + 0.003 * var15;
            var11[var14] = 0.0022 + 0.0016 * var17 + 8.0E-4 * var15;
            var12[var14] = 0.0062 + 0.0055 * var17 + 0.0025 * (1.0 - var19) + 0.005 * var15;
            var13[var14] = 0.0;
        }

        Array<double> var110 = var4[5];
        Array<double> var111 = var4[6];
        Array<double> var16 = var4[7];
        Array<double> var112 = var4[8];
        Array<double> var18 = var4[9];
        Array<double> var113 = var4[10];
        double var20 = NX;
        double var22 = NY;
        double var24 = NZ;
        double var26 = E1X;
        double var28 = E1Y;
        double var30 = E1Z;
        double var32 = E2X;
        double var34 = E2Y;
        double var36 = E2Z;

        for (int var38 = var5; var38 < var6; var38++) {
            if ((var3[var38] & 1) != 0) {
                double var39 = var0[var38];
                double var41 = var1[var38];
                double var43 = var2[var38];
                double var45 = var39 * var20 + var41 * var22 + var43 * var24;
                double var47 = Math::asin(var45 > 1.0 ? 1.0 : (var45 < -1.0 ? -1.0 : var45));
                double var49 = var39 * var26 + var41 * var28 + var43 * var30;
                double var51 = var39 * var32 + var41 * var34 + var43 * var36;
                double var53 = Math::atan2(var51, var49);
                double var55 = Math::acos(var49 > 1.0 ? 1.0 : (var49 < -1.0 ? -1.0 : var49));
                double var57 = var55 * var55;
                double var59 = var47 + 0.06 * var110[var38];
                double var61 = 0.085 + 0.12 * Math::exp(-var53 * var53 / 0.3);
                double var63 = Math::exp(-var59 * var59 / (var61 * var61));
                double var65 = Math::exp(-var47 * var47 / 0.09);
                double var67 = (var111[var38] - 0.4) * 2.272727272727273;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var69 = var67 * var67 * (3.0 - 2.0 * var67);
                double var71 = var16[var38];
                double var73 = var69 * Math::sqrt(var69) * (0.25 + 1.4 * var71 * var71);
                double var75 = var112[var38];
                double var77 = 1.0 - Math::abs(2.0 * var75 - 1.0);
                var77 *= var77;
                var77 *= var77;
                double var79 = var18[var38];
                var67 = (var79 - 0.56) * 7.142857142857142;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var81 = var67 * var67 * (3.0 - 2.0 * var67);
                double var83 = var59 - 0.03 * Math::sin(var53 * 1.7 + 0.5) - 0.012 * Math::sin(var53 * 5.1 + 1.3) -
                               0.03 * (var79 - 0.5);
                var67 = (var79 - 0.3) * 2.0;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var85 = 0.018 + 0.022 * var67 * var67 * (3.0 - 2.0 * var67);
                var67 = (var75 - 0.3) * 2.2222222222222223;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var87 =
                    Math::exp(-var83 * var83 / (var85 * var85)) * (0.55 + 0.45 * var67 * var67 * (3.0 - 2.0 * var67));
                double var89 = 0.85 * var77 * var63 + 0.6 * var81 * var63 + 0.95 * var87 * Math::exp(-var57 / 2.5);
                if (var89 > 1.0) {
                    var89 = 1.0;
                }

                double var91 = var57 < 3.0 ? Math::exp(-var57 / 0.11560000000000002) : 0.0;
                double var93 = var57 < 0.3 ? Math::exp(-var57 / 0.010000000000000002) : 0.0;
                double var95 = var63 * (0.008 + 0.38 * var73) + 0.005 * var65 * (0.5 + var69) +
                               var91 * (0.3 * var63 * (0.5 + 0.8 * var69) + 0.035) + 0.42 * var93 * var63;
                var95 *= 1.0 - 0.97 * var89;
                double var97 = var91 * 1.25;
                if (var97 > 1.0) {
                    var97 = 1.0;
                }

                double var99 = var10[var38] + var95 * (0.72 + 0.28 * var97) * (1.0 + 0.08 * var89);
                double var101 = var11[var38] + var95 * (0.8 + 0.06 * var97) * (1.0 - 0.1 * var89);
                double var103 = var12[var38] + var95 * (1.0 - 0.4 * var97) * (1.0 - 0.25 * var89);
                var67 = (var113[var38] - 0.72) * 6.25;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var105 = var67 * var67 * (3.0 - 2.0 * var67);
                var67 = (var89 - 0.15) * 2.857142857142857;
                var67 = var67 <= 0.0 ? 0.0 : (var67 >= 1.0 ? 1.0 : var67);
                double var107 = var105 * var63 * var67 * var67 * (3.0 - 2.0 * var67) * (1.0 - var89);
                var10[var38] = var99 + var107 * 0.2;
                var11[var38] = var101 + var107 * 0.03;
                var12[var38] = var103 + var107 * 0.1;
                var13[var38] = var63 * (0.4 + 0.6 * var69) * (1.0 - 0.8 * var89);
            }
        }
    } catch (...) {
        throw;
    }
}

void f(int var0, Array<double> var1, Array<double> var2, Array<double> var3, Array<std::int8_t> var4,
       Array<Array<double>> var5, int var6, int var7) {

    try {
        int var8 = var0 * 13;
        int var9 = 2 << var0;
        double var10 = NEB[var8];
        double var12 = NEB[var8 + 1];
        double var14 = NEB[var8 + 2];
        double var16 = NEB[var8 + 3];
        double var18 = NEB[var8 + 4];
        double var20 = NEB[var8 + 5];
        double var22 = NEB[var8 + 6];
        double var24 = NEB[var8 + 7];
        double var26 = NEB[var8 + 8];
        double var28 = NEB[var8 + 9];
        double var30 = NEB[var8 + 12];
        double var32 = 1.0 / var16;
        double var34 = 1.0 / (var16 * var16 * 0.1);
        Array<double> var36 = var5[15];
        Array<double> var37 = var5[17];
        Array<double> var38 = var5[18];
        Array<double> var39 = var5[19];
        Array<double> var40 = var5[20];
        Array<double> var41 = var5[11];
        Array<double> var42 = var5[12];
        Array<double> var43 = var5[13];
        Array<double> var44 = var5[14];

        for (int var45 = var6; var45 < var7; var45++) {
            if ((var4[var45] & var9) != 0) {
                double var46 = var1[var45] * var10 + var2[var45] * var12 + var3[var45] * var14;
                double var48 = Math::acos(var46 > 1.0 ? 1.0 : (var46 < -1.0 ? -1.0 : var46));
                double var50 = var48 * var32 * (1.0 + 1.1 * var36[var45]);
                double var52 = Math::exp(-var50 * var50 * 1.3);
                double var54 = var37[var45];
                double var56 = 1.0 - Math::abs(2.0 * var38[var45] - 1.0);
                var56 *= var56;
                var56 *= var56;
                double var58 = (var54 - 0.42) * 2.3255813953488373;
                var58 = var58 <= 0.0 ? 0.0 : (var58 >= 1.0 ? 1.0 : var58);
                double var60 = var58 * var58 * (3.0 - 2.0 * var58);
                var58 = (var54 - 0.3) * 2.857142857142857;
                var58 = var58 <= 0.0 ? 0.0 : (var58 >= 1.0 ? 1.0 : var58);
                double var62 = var58 * var58 * (3.0 - 2.0 * var58);
                var58 = (var54 - 0.45) * 2.857142857142857;
                var58 = var58 <= 0.0 ? 0.0 : (var58 >= 1.0 ? 1.0 : var58);
                double var64 = var58 * var58 * (3.0 - 2.0 * var58);
                double var66 = var52 * (0.45 * var60 + 0.85 * var56 * var62);
                var58 = (var39[var45] - 0.58) * 6.25;
                var58 = var58 <= 0.0 ? 0.0 : (var58 >= 1.0 ? 1.0 : var58);
                double var68 = var58 * var58 * (3.0 - 2.0 * var58) * var52;
                var58 = (var40[var45] - 0.3) * 2.2222222222222223;
                var58 = var58 <= 0.0 ? 0.0 : (var58 >= 1.0 ? 1.0 : var58);
                double var70 = var58 * var58 * (3.0 - 2.0 * var58);
                double var72 = Math::exp(-var48 * var48 * var34);
                double var74 = var18 + (var24 - var18) * var70;
                double var76 = var20 + (var26 - var20) * var70;
                double var78 = var22 + (var28 - var22) * var70;
                double var80 = var30 * var66 * 0.5;
                double var82 = var56 * var52 * var64 * 0.1;
                double var84 = 1.0 - 0.75 * var68;
                var41[var45] =
                    var41[var45] * var84 + (var80 * var74 + var72 * var66 * 0.16 + var82 * (0.6 + 0.4 * var74)) * var84;
                var42[var45] =
                    var42[var45] * var84 + (var80 * var76 + var72 * var66 * 0.15 + var82 * (0.6 + 0.4 * var76)) * var84;
                var43[var45] =
                    var43[var45] * var84 + (var80 * var78 + var72 * var66 * 0.2 + var82 * (0.6 + 0.4 * var78)) * var84;
                var44[var45] += 0.5 * var66;
            }
        }
    } catch (...) {
        throw;
    }
}

void g(Array<int> var0, int var1, int var2, Array<double> var3, Array<double> var4, Array<double> var5,
       Array<double> var6, Array<Array<double>> var7) {

    try {
        Array<double> var8 = var7[11];
        Array<double> var9 = var7[12];
        Array<double> var10 = var7[13];
        Array<double> var11 = var7[14];
        Array<float> var12 = ae;
        Array<double> var13 = af;
        int var14 = (var1 / 3 * (938516865 ^ 938517893) + var2) * (-1798495352 ^ -1798492284) +
                    var1 % 3 * (937270334 ^ 937269306);
        int var15 = var2 & 3;

        for (int var16 = 0; var16 < (-1443739262 ^ -1443738234); var16++) {
            double var17 = 450.56000000000006;
            double var19 = var3[var16] * var17;
            double var21 = var4[var16] * var17;
            double var23 = var5[var16] * var17;
            int var25 = (int)Math::floor(var19 - 0.5);
            int var26 = (int)Math::floor(var21 - 0.5);
            int var27 = (int)Math::floor(var23 - 0.5);
            double var28 = var11[var16];
            double var30 = 0.07 + 0.55 * (var28 < 1.0 ? var28 : 1.0);
            double var32 = 0.0;
            double var34 = 0.0;
            double var36 = 0.0;

            for (int var38 = 0; var38 < 8; var38++) {
                int var39 = var25 + (var38 & 1);
                int var40 = var26 + (var38 >> 1 & 1);
                int var41 = var27 + (var38 >> 2 & 1);
                int var42 = var39 * (912328550 ^ -1144540123) ^ var40 * (694317993 ^ -244036632) ^
                            var41 * (2043982238 ^ -1295121791);
                int var43 = var42 ^ 97373115 ^ 328969226;
                var43 ^= (static_cast<std::uint32_t>(var43) >> 15);
                var43 *= 1575843689 ^ 1911966468;
                var43 ^= (static_cast<std::uint32_t>(var43) >> 12);
                var43 *= 1838025220 ^ 1156851517;
                var43 ^= (static_cast<std::uint32_t>(var43) >> 15);
                if (!((double)((static_cast<std::uint32_t>(var43) >> 8)) * 5.9604645E-8F > var30)) {
                    int var44 = var42 ^ -845253282 ^ -516769220;
                    var44 ^= (static_cast<std::uint32_t>(var44) >> 15);
                    var44 *= -845717991 ^ -510893452;
                    var44 ^= (static_cast<std::uint32_t>(var44) >> 12);
                    var44 *= 214224248 ^ 633266241;
                    var44 ^= (static_cast<std::uint32_t>(var44) >> 15);
                    int var45 = var42 ^ 1337221035 ^ 213346488;
                    var45 ^= (static_cast<std::uint32_t>(var45) >> 15);
                    var45 *= 1941558431 ^ 1604511986;
                    var45 ^= (static_cast<std::uint32_t>(var45) >> 12);
                    var45 *= 844539190 ^ 455903247;
                    var45 ^= (static_cast<std::uint32_t>(var45) >> 15);
                    double var46 =
                        var19 - ((double)var39 +
                                 (double)((static_cast<std::uint32_t>(var44) >> 8) & (470041767 ^ 470041688)) / 255.0);
                    double var48 =
                        var21 - ((double)var40 +
                                 (double)((static_cast<std::uint32_t>(var44) >> 16) & (202766400 ^ 202766527)) / 255.0);
                    double var50 = var23 - ((double)var41 + (double)((static_cast<std::uint32_t>(var45) >> 8) &
                                                                     (1694133270 ^ 1694133481)) /
                                                                255.0);
                    double var52 =
                        (double)((static_cast<std::uint32_t>(var45) >> 16) & (-346619153 ^ -346555120)) / 65535.0;
                    double var54 = var52 * var52;
                    double var56 = var54 * var54;
                    double var58 = Math::exp(-(var46 * var46 + var48 * var48 + var50 * var50) * 5.2) *
                                   (0.025 + 0.35 * var56 * var56);
                    int var60 = (static_cast<std::uint32_t>(var45) >> 4) & 7;
                    if (var60 < 2) {
                        var32 += var58 * 0.72;
                        var34 += var58 * 0.84;
                        var36 += var58;
                    } else if (var60 < 6) {
                        var32 += var58;
                        var34 += var58 * 0.96;
                        var36 += var58 * 0.9;
                    } else {
                        var32 += var58;
                        var34 += var58 * 0.78;
                        var36 += var58 * 0.58;
                    }
                }
            }

            double var62 = var13[(var16 & 3) << 2 | var15];
            double var63 = var8[var16] + var32;
            double var66 = var9[var16] + var34;
            double var79 = var10[var16] + var36;
            var63 /= 1.0 + 0.35 * var63;
            var66 /= 1.0 + 0.35 * var66;
            var79 /= 1.0 + 0.35 * var79;
            var63 = var63 >= 1.0 ? 1.0 : (var63 <= 0.0 ? 0.0 : var63);
            var66 = var66 >= 1.0 ? 1.0 : (var66 <= 0.0 ? 0.0 : var66);
            var79 = var79 >= 1.0 ? 1.0 : (var79 <= 0.0 ? 0.0 : var79);
            int var87 = (int)((double)var12[(int)(Math::sqrt(var63) * 4096.0 + 0.5)] + var62);
            int var47 = (int)((double)var12[(int)(Math::sqrt(var66) * 4096.0 + 0.5)] + var62);
            int var88 = (int)((double)var12[(int)(Math::sqrt(var79) * 4096.0 + 0.5)] + var62);
            var0[var14 + var16] = 815719095 ^ -811670857 |
                                  (var87 > (-183077108 ^ -183076877) ? -1655235291 ^ -1655235110 : var87) << 16 |
                                  (var47 > (-673291331 ^ -673291454) ? -76744849 ^ -76744816 : var47) << 8 |
                                  (var88 > (-1188978033 ^ -1188978064) ? 744040472 ^ 744040679 : var88);
        }
    } catch (...) {
        throw;
    }
}

Array<int> h() {

    Array<int> var0 = Array<int>(1048576);
    i(var0);
    m(var0);
    p(var0);
    return var0;
}

void i(Array<int> var0) {

    int var1 = 3;
    int var2 = -428379932 ^ -428376860;
    Array<double> var3 = Array<double>(3072);
    Array<double> var4 = Array<double>(3072);
    Array<double> var5 = Array<double>(3072);
    Array<double> var6 = Array<double>(3072);
    Array<double> var7 = Array<double>(3072);
    Array<std::int8_t> var8 = Array<std::int8_t>(3072);
    Array<int> var9 = Array<int>(2);
    Array<double> var10 = Array<double>(4096);
    Array<float> var11 = k();

    for (int var12 = 0; var12 < (365939952 ^ 365940464); var12++) {
        Arrays::fill(var10, 0.0);

        for (int var13 = 0; var13 < 3; var13++) {
            double var14 = (256.0 - ((double)var12 + ((double)var13 + 0.5) / 3.0)) / 176.0;
            j(var14, 512.0, 176.0, 3, var3, var4, var5, var8, var9);
            if (var9[0] < var9[1]) {
                d(var3, var4, var5, var8, 1, var9[0], var9[1], 3.0, 11.0, 5.0, 2.0, nullptr, nullptr, 0.0, 4, 0.5,
                  var6);
                d(var3, var4, var5, var8, 1, var9[0], var9[1], 9.0, 3.0, 17.0, 8.0, nullptr, nullptr, 0.0, 3, 0.5,
                  var7);
            }

            l(var14, var3, var5, var8, var6, var7, var11, var10);
        }

        o(var0, var12 * (577906515 ^ 577905491), var10, -277302324 ^ -277301300, 0.1111111111111111);
    }
}

void j(double var0, double var2, double var4, int var6, Array<double> var7, Array<double> var8, Array<double> var9,
       Array<std::int8_t> var10, Array<int> var11) {

    try {
        int var12 = var7.size();
        int var13 = var12;
        int var14 = 0;
        double var15 = 1.0 / (double)var6;
        double var17 = 1.0 / var4;

        for (int var19 = 0; var19 < var12; var19++) {
            double var20 = ((double)(var19 / var6) + ((double)(var19 % var6) + 0.5) * var15 - var2) * var17;
            double var22 = var20 * var20 + var0 * var0;
            var7[var19] = var20;
            var8[var19] = var0;
            if (var22 < 1.0) {
                var9[var19] = Math::sqrt(1.0 - var22);
                var10[var19] = 1;
                if (var19 < var13) {
                    var13 = var19;
                }

                var14 = var19 + 1;
            } else {
                var9[var19] = 0.0;
                var10[var19] = 0;
            }
        }

        var11[0] = var13;
        var11[1] = var14;
    } catch (...) {
        throw;
    }
}

Array<float> k() {

    Array<float> var0 = Array<float>(4096);
    Array<float> var1 = ad;

    for (int var2 = 0; var2 < (984583292 ^ 984587388); var2++) {
        double var3 = (double)var2 / 4095.0;
        if (!(var3 <= 0.0) && !(var3 >= 1.0)) {
            double var5 = var3 * 160.0;
            double var7 = 3.3;
            double var9 = 7.1;
            int var11 = (int)var5;
            int var12 = 3;
            int var13 = 7;
            double var14 = var5 - (double)var11;
            double var16 = var7 - (double)var12;
            double var18 = var9 - (double)var13;
            var14 = var14 * var14 * (3.0 - 2.0 * var14);
            var16 = var16 * var16 * (3.0 - 2.0 * var16);
            var18 = var18 * var18 * (3.0 - 2.0 * var18);
            int var20 = var11 & 63;
            int var21 = var11 + 1 & 63;
            int var22 = var12 << 6;
            int var23 = var12 + 1 << 6;
            int var24 = var13 << 12;
            int var25 = var13 + 1 << 12;
            double var26 = (double)var1[var24 | var22 | var20];
            double var28 = (double)var1[var24 | var23 | var20];
            double var30 = (double)var1[var25 | var22 | var20];
            double var32 = (double)var1[var25 | var23 | var20];
            var26 += ((double)var1[var24 | var22 | var21] - var26) * var14;
            var28 += ((double)var1[var24 | var23 | var21] - var28) * var14;
            var30 += ((double)var1[var25 | var22 | var21] - var30) * var14;
            var32 += ((double)var1[var25 | var23 | var21] - var32) * var14;
            var26 += (var28 - var26) * var16;
            var30 += (var32 - var30) * var16;
            double var34 = var26 + (var30 - var26) * var18;
            double var36 = 0.62 + 0.2 * Math::sin(var3 * 47.0) * Math::sin(var3 * 13.0 + 1.0) + 0.14 * (var34 - 0.5);
            double var38 = (var3 - 0.1) * 5.0;
            var38 = var38 <= 0.0 ? 0.0 : (var38 >= 1.0 ? 1.0 : var38);
            var36 *= 0.3 + 0.7 * var38 * var38 * (3.0 - 2.0 * var38);
            double var40 = (var3 - 0.63) / 0.02;
            var36 *= 1.0 - 0.93 * Math::exp(-var40 * var40);
            var40 = (var3 - 0.885) / 0.006;
            var36 *= 1.0 - 0.8 * Math::exp(-var40 * var40);
            var38 = var3 * 40.0;
            var38 = var38 >= 1.0 ? 1.0 : var38;
            double var42 = (var3 - 0.95) * 20.0;
            var42 = var42 <= 0.0 ? 0.0 : (var42 >= 1.0 ? 1.0 : var42);
            var36 *= var38 * var38 * (3.0 - 2.0 * var38) * (1.0 - var42 * var42 * (3.0 - 2.0 * var42));
            var0[var2] = (float)(var36 <= 0.0 ? 0.0 : (var36 >= 1.0 ? 1.0 : var36));
        }
    }

    return var0;
}

void l(double var0, Array<double> var2, Array<double> var3, Array<std::int8_t> var4, Array<double> var5,
       Array<double> var6, Array<float> var7, Array<double> var8) {

    try {
        double var9 = v;
        double var11 = w;
        double var13 = x;
        double var15 = y;
        double var17 = z;
        double var19 = aa;
        double var21 = var15 * var9 + var17 * var11 + var19 * var13;
        bool var23 = var21 > 0.0 == var19 > 0.0;
        double var24 = 4095.000000000001;

        for (int var26 = 0; var26 < (1326203675 ^ 1326206747); var26++) {
            double var27 = var2[var26];
            double var29 = -(var27 * var15 + var0 * var17) / var19;
            double var31 = Math::sqrt(var27 * var27 + var0 * var0 + var29 * var29);
            double var33 = 0.0;
            double var35 = 0.0;
            double var37 = 0.0;
            double var39 = 0.0;
            if (var31 > 1.28 && var31 < 2.28) {
                double var41 = (var31 - 1.28) / 0.9999999999999998;
                var33 = (double)var7[(int)((var31 - 1.28) * var24 + 0.5)] * 0.92;
                if (var33 > 0.0) {
                    double var43 = var23 ? 0.45 + 0.55 * Math::abs(var21) : 0.18 + 0.3 * (1.0 - var33);
                    double var45 = -(var27 * var9 + var0 * var11 + var29 * var13);
                    if (var45 > 0.0) {
                        double var47 = var27 + var9 * var45;
                        double var49 = var0 + var11 * var45;
                        double var51 = var29 + var13 * var45;
                        if (var47 * var47 + var49 * var49 + var51 * var51 < 1.0) {
                            var43 *= 0.07;
                        }
                    }

                    var35 = (0.86 - 0.16 * var41) * var43;
                    var37 = (0.8 - 0.18 * var41) * var43;
                    var39 = (0.74 + 0.1 * var41) * var43;
                }
            }

            double var110;
            double var111;
            double var112;
            double var113;
            if (var4[var26] != 0) {
                double var53 = var3[var26];
                double var55 = var27 * var15 + var0 * var17 + var53 * var19;
                double var57 = Math::asin(var55 > 1.0 ? 1.0 : (var55 < -1.0 ? -1.0 : var55));
                double var59 = var5[var26];
                double var61 = var6[var26];
                double var63 = var57 + 0.045 * var59 + 0.012 * var61;
                double var65 = Math::sin(var63 * 11.0 + 0.8);
                double var67 = Math::sin(var63 * 29.0 + 2.0 + 2.0 * var59);
                double var69 = (var65 + 0.3) * 1.1111111111111112;
                var69 = var69 <= 0.0 ? 0.0 : (var69 >= 1.0 ? 1.0 : var69);
                var69 = var69 * var69 * (3.0 - 2.0 * var69);
                double var71 = 0.6 + 0.33 * var69;
                double var73 = 0.5 + 0.16 * var69;
                double var75 = 0.78 - 0.16 * var69;
                var69 = (var67 - 0.4) * 1.8181818181818181;
                var69 = var69 <= 0.0 ? 0.0 : (var69 >= 1.0 ? 1.0 : var69);
                double var77 = 0.4 * var69 * var69 * (3.0 - 2.0 * var69);
                var71 += (0.36 - var71) * var77;
                var73 += (0.26 - var73) * var77;
                var75 += (0.56 - var75) * var77;
                var69 = (-var67 - 0.55) * 2.2222222222222223;
                var69 = var69 <= 0.0 ? 0.0 : (var69 >= 1.0 ? 1.0 : var69);
                double var79 = var65 * 2.0;
                var79 = var79 <= 0.0 ? 0.0 : (var79 >= 1.0 ? 1.0 : var79);
                double var81 = 0.3 * var69 * var69 * (3.0 - 2.0 * var69) * var79 * var79 * (3.0 - 2.0 * var79);
                var71 += (0.97 - var71) * var81;
                var73 += (0.9 - var73) * var81;
                var75 += (0.82 - var75) * var81;
                double var83 = var27 + 0.25;
                double var85 = var0 + 0.42;
                double var87 = var53 - 0.87;
                double var89 = var83 * var83 / 0.02 + var85 * var85 / 0.006 + var87 * var87 / 0.02;
                if (var89 < 40.0) {
                    double var91 = Math::exp(-var89) *
                                   (0.75 + 0.25 * Math::sin(Math::atan2(var85, var83) * 3.0 + var89 * 4.0)) * 0.8;
                    var71 += (0.96 - var71) * var91;
                    var73 += (0.52 - var73) * var91;
                    var75 += (0.44 - var75) * var91;
                }

                double var137 = var27 * var9 + var0 * var11 + var53 * var13;
                double var93 = var137 > 0.0 ? var137 : 0.0;
                double var95 = Math::sqrt(var93);
                var69 = (var137 + 0.1) * 3.125;
                var69 = var69 <= 0.0 ? 0.0 : (var69 >= 1.0 ? 1.0 : var69);
                double var97 = var69 * var69 * (3.0 - 2.0 * var69) * (0.16 + 0.84 * var95 * Math::sqrt(var95));
                var97 *= 0.62 + 0.38 * Math::pow(var53, 0.6);
                double var99 = -(var27 * var15 + var0 * var17 + var53 * var19) / var21;
                if (var99 > 0.0) {
                    double var101 = var27 + var9 * var99;
                    double var103 = var0 + var11 * var99;
                    double var105 = var53 + var13 * var99;
                    double var107 = Math::sqrt(var101 * var101 + var103 * var103 + var105 * var105);
                    if (var107 > 1.28 && var107 < 2.28) {
                        var97 *= 1.0 - 0.8 * (double)var7[(int)((var107 - 1.28) * var24 + 0.5)];
                    }
                }

                double var139 = 1.0 - var53;
                var69 = (var137 + 0.2) * 1.5384615384615383;
                var69 = var69 <= 0.0 ? 0.0 : (var69 >= 1.0 ? 1.0 : var69);
                double var140 = var139 * var139 * Math::sqrt(var139) * var69 * var69 * (3.0 - 2.0 * var69) * 0.55;
                var110 = var71 * (var97 + 0.012) + var140 * 0.55;
                var111 = var73 * (var97 + 0.012) + var140 * 0.66;
                var112 = var75 * (var97 + 0.012) + var140;
                var113 = 1.0;
                if (var33 > 0.0 && var29 > var53) {
                    var110 = var35 * var33 + var110 * (1.0 - var33);
                    var111 = var37 * var33 + var111 * (1.0 - var33);
                    var112 = var39 * var33 + var112 * (1.0 - var33);
                }
            } else {
                double var114 = Math::sqrt(var27 * var27 + var0 * var0);
                double var116 = 0.0;
                if (var114 < 1.6) {
                    double var117 = (var27 * var9 + var0 * var11) / Math::max(var114, 1.0E-6);
                    double var118 = (var117 + 0.35) * 1.0526315789473684;
                    var118 = var118 <= 0.0 ? 0.0 : (var118 >= 1.0 ? 1.0 : var118);
                    var116 = Math::exp(-(var114 - 1.0) / 0.03) * var118 * var118 * (3.0 - 2.0 * var118) * 0.8;
                    if (var116 > 1.0) {
                        var116 = 1.0;
                    }
                }

                var110 = 0.55 * var116;
                var111 = 0.68 * var116;
                var112 = var116;
                var113 = var116 * 0.9;
                if (var33 > 0.0) {
                    var110 = var35 * var33 + var110 * (1.0 - var33);
                    var111 = var37 * var33 + var111 * (1.0 - var33);
                    var112 = var39 * var33 + var116 * (1.0 - var33);
                    var113 = var33 + var113 * (1.0 - var33);
                }
            }

            int var115 = var26 / 3 * 4;
            var8[var115] += var110;
            var8[var115 + 1] = var8[var115 + 1] + var111;
            var8[var115 + 2] = var8[var115 + 2] + var112;
            var8[var115 + 3] = var8[var115 + 3] + var113;
        }
    } catch (...) {
        throw;
    }
}

void m(Array<int> var0) {

    int var1 = 2;
    int var2 = 850758691 ^ 850759203;
    int var3 = 40;
    Array<double> var4 = Array<double>(512);
    Array<double> var5 = Array<double>(512);
    Array<double> var6 = Array<double>(512);
    Array<double> var7 = Array<double>(512);
    Array<double> var8 = Array<double>(512);
    Array<std::int8_t> var9 = Array<std::int8_t>(512);
    Array<double> var10 = Array<double>(1024);
    Array<double> var11 = Array<double>(160);

    for (int var12 = 0; var12 < 40; var12++) {
        int var13 = var12 * (798450288 ^ -1573793485) ^ 2132795084 ^ -1752934645 ^ -1874415198 ^ -250173185 ^
                    -1873895939 ^ 1497839812;
        var13 ^= (static_cast<std::uint32_t>(var13) >> 15);
        var13 *= -757833653 ^ -19964890;
        var13 ^= (static_cast<std::uint32_t>(var13) >> 12);
        var13 *= 1438949418 ^ 2092862739;
        var13 ^= (static_cast<std::uint32_t>(var13) >> 15);
        int var14 = var12 * (246891986 ^ -2095974767) ^ 1030981416 ^ -446316695 ^ -2112888774 ^ 1972019105 ^
                    1856507369 ^ 1107608203;
        var14 ^= (static_cast<std::uint32_t>(var14) >> 15);
        var14 *= -2131246310 ^ -1393754249;
        var14 ^= (static_cast<std::uint32_t>(var14) >> 12);
        var14 *= 2096620792 ^ 1435355585;
        var14 ^= (static_cast<std::uint32_t>(var14) >> 15);
        double var15 =
            (double)((static_cast<std::uint32_t>(var13) >> 8) & (1601664116 ^ 1601665931)) / 4095.0 * 2.0 - 1.0;
        double var17 =
            (double)((static_cast<std::uint32_t>(var13) >> 20) & (-1119485746 ^ -1119482063)) / 4095.0 * Math::PI * 2.0;
        double var19 = Math::sqrt(1.0 - var15 * var15);
        double var21 = (double)((static_cast<std::uint32_t>(var14) >> 8) & (292624602 ^ 292624165)) / 4095.0;
        var11[4 * var12] = var19 * Math::cos(var17);
        var11[4 * var12 + 1] = var19 * Math::sin(var17);
        var11[4 * var12 + 2] = var15;
        var11[4 * var12 + 3] = 0.03 + 0.2 * var21 * var21 * var21;
    }

    for (int var23 = 0; var23 < (1872424030 ^ 1872424286); var23++) {
        Arrays::fill(var10, 0.0);

        for (int var29 = 0; var29 < 2; var29++) {
            double var35 = (128.0 - ((double)var23 + ((double)var29 + 0.5) / 2.0)) / 120.0;
            int var16 = 1086007672 ^ 1086008184;
            int var36 = 0;

            for (int var18 = 0; var18 < (1917060573 ^ 1917061085); var18++) {
                double var37 = ((double)(var18 >> 1) + ((double)(var18 & 1) + 0.5) / 2.0 - 128.0) / 120.0;
                double var38 = var37 * var37 + var35 * var35;
                var4[var18] = var37;
                var5[var18] = var35;
                if (var38 < 1.0) {
                    var6[var18] = Math::sqrt(1.0 - var38);
                    var9[var18] = 1;
                    if (var18 < var16) {
                        var16 = var18;
                    }

                    var36 = var18 + 1;
                } else {
                    var6[var18] = 0.0;
                    var9[var18] = 0;
                }
            }

            if (var16 < var36) {
                d(var4, var5, var6, var9, 1, var16, var36, 2.2, 40.0, 3.0, 9.0, nullptr, nullptr, 0.0, 4, 0.0, var7);
                d(var4, var5, var6, var9, 1, var16, var36, 14.0, 1.0, 2.0, 3.0, nullptr, nullptr, 0.0, 3, 0.0, var8);
            }

            n(var35, var4, var6, var9, var7, var8, var11, var10);
        }

        o(var0, ((1171457647 ^ 1171457135) + var23) * (-1376175173 ^ -1376174149), var10, -477688989 ^ -477689245,
          0.25);
    }
}

void n(double var0, Array<double> var2, Array<double> var3, Array<std::int8_t> var4, Array<double> var5,
       Array<double> var6, Array<double> var7, Array<double> var8) {

    try {
        double var9 = v;
        double var11 = w;
        double var13 = x;

        for (int var15 = 0; var15 < (-855658846 ^ -855659358); var15++) {
            if (var4[var15] != 0) {
                double var16 = var2[var15];
                double var18 = var3[var15];
                double var20 = (var5[var15] - 0.48) * 7.142857142857142;
                var20 = var20 <= 0.0 ? 0.0 : (var20 >= 1.0 ? 1.0 : var20);
                double var22 = 0.62 * (1.0 - 0.28 * var20 * var20 * (3.0 - 2.0 * var20)) * (0.92 + 0.16 * var6[var15]);

                for (int var24 = 0; var24 < (1951273122 ^ 1951272962); var24 += 4) {
                    double var25 = var16 - var7[var24];
                    double var27 = var0 - var7[var24 + 1];
                    double var29 = var18 - var7[var24 + 2];
                    double var31 = var7[var24 + 3];
                    double var33 = var25 * var25 + var27 * var27 + var29 * var29;
                    if (var33 < 1.96 * var31 * var31) {
                        double var35 = Math::sqrt(var33) / var31;
                        double var37 = (var35 - 1.0) / 0.16;
                        var22 *=
                            1.0 - 0.14 * (var35 < 1.0 ? 1.0 - var35 * var35 : 0.0) + 0.12 * Math::exp(-var37 * var37);
                    }
                }

                double var43 = var16 * var9 + var0 * var11 + var18 * var13;
                var20 = (var43 + 0.06) * 4.166666666666667;
                var20 = var20 <= 0.0 ? 0.0 : (var20 >= 1.0 ? 1.0 : var20);
                double var26 =
                    var20 * var20 * (3.0 - 2.0 * var20) * (0.1 + 0.9 * Math::pow(var43 > 0.0 ? var43 : 0.0, 0.8));
                double var28 = var22 * (var26 + 0.01);
                int var30 = (var15 >> 1) * 4;
                var8[var30] += var28 * 0.96;
                var8[var30 + 1] = var8[var30 + 1] + var28 * 0.95;
                var8[var30 + 2] = var8[var30 + 2] + var28 * 1.04;
                var8[var30 + 3]++;
            }
        }
    } catch (...) {
        throw;
    }
}

void o(Array<int> var0, int var1, Array<double> var2, int var3, double var4) {

    try {
        Array<float> var6 = ae;

        for (int var7 = 0; var7 < var3; var7++) {
            int var8 = var7 * 4;
            double var9 = var2[var8 + 3] * var4;
            var9 = var9 <= 0.0 ? 0.0 : (var9 >= 1.0 ? 1.0 : var9);
            int var11 = (int)(var9 * 255.0 + 0.5);
            int var12 = 0;
            int var13 = 0;
            int var14 = 0;
            if (var11 > 0) {
                double var15 = var4 / var9;
                double var17 = var2[var8] * var15;
                double var19 = var2[var8 + 1] * var15;
                double var21 = var2[var8 + 2] * var15;
                var17 = var17 <= 0.0 ? 0.0 : (var17 >= 1.0 ? 1.0 : var17);
                var19 = var19 <= 0.0 ? 0.0 : (var19 >= 1.0 ? 1.0 : var19);
                var21 = var21 <= 0.0 ? 0.0 : (var21 >= 1.0 ? 1.0 : var21);
                var12 = (int)((double)(var6[(int)(Math::sqrt(var17) * 4096.0 + 0.5)] * (float)var11) / 255.0 + 0.5);
                var13 = (int)((double)(var6[(int)(Math::sqrt(var19) * 4096.0 + 0.5)] * (float)var11) / 255.0 + 0.5);
                var14 = (int)((double)(var6[(int)(Math::sqrt(var21) * 4096.0 + 0.5)] * (float)var11) / 255.0 + 0.5);
            }

            var0[var1 + var7] = var11 << 24 | var12 << 16 | var13 << 8 | var14;
        }
    } catch (...) {
        throw;
    }
}

void p(Array<int> var0) {

    for (int var1 = 0; var1 < (-806399530 ^ -806399786); var1++) {
        double var2 = ((double)var1 + 0.5) / 128.0 - 1.0;

        for (int var4 = 0; var4 < (295548328 ^ 295548072); var4++) {
            double var5 = ((double)var4 + 0.5) / 128.0 - 1.0;
            double var7 = Math::sqrt(var5 * var5 + var2 * var2) / 0.955;
            double var9 = 0.0;
            if (var7 < 1.0) {
                double var11 = 1.0 - var7 * var7;
                var9 = 0.2 * Math::exp(-var7 * var7 / 0.0128) * var11 +
                       0.8 * var11 * var11 * var11 / (1.0 + 5.0 * var7 * var7);
            }

            int var49 = (int)((var9 >= 1.0 ? 1.0 : var9) * 255.0 + 0.5);
            var0[((140885420 ^ 140885932) + var1) * (1930516082 ^ 1930515058) + (2124321214 ^ 2124320958) + var4] =
                var49 * (-1278098980 ^ -1294941987);
        }
    }

    for (int var32 = 0; var32 < 3; var32++) {
        int var35 = var32 == 0 ? 8 : (var32 == 1 ? 32 : 64);
        int var3 =
            var32 == 0 ? -1135095546 ^ -1135095034 : (var32 == 1 ? -1459764954 ^ -1459764426 : 1016275658 ^ 1016275082);
        double var38 = (double)var35 * 0.5;

        for (int var6 = 0; var6 < var35; var6++) {
            double var44 = ((double)var6 + 0.5 - var38) / var38;

            for (int var46 = 0; var46 < var35; var46++) {
                double var10 = ((double)var46 + 0.5 - var38) / var38;
                double var12 = var10 * var10 + var44 * var44;
                double var14 = Math::sqrt(var12);
                double var18 = 0.0;
                double var20 = (var14 - 0.78) * 4.545454545454546;
                var20 = var20 <= 0.0 ? 1.0 : (var20 >= 1.0 ? 0.0 : 1.0 - var20 * var20 * (3.0 - 2.0 * var20));
                double var16;
                if (var32 == 0) {
                    var16 = Math::exp(-var12 / 0.32000000000000006) * var20;
                } else if (var32 == 1) {
                    var16 = Math::exp(-var12 / 0.18) * var20;
                    var18 = 0.25 * Math::exp(-var14 / 0.3) * var20;
                } else {
                    double var22 = 1.0 - Math::abs(var10);
                    double var24 = 1.0 - Math::abs(var44);
                    double var26 = var44 / 0.022;
                    double var28 = var10 / 0.022;
                    double var30 = 0.22 * (Math::exp(-var26 * var26) * var22 * var22 * Math::sqrt(var22) +
                                           Math::exp(-var28 * var28) * var24 * var24 * Math::sqrt(var24));
                    var16 = Math::exp(-var12 / 0.020000000000000004) * var20 + var30;
                    var18 = 0.25 * Math::exp(-var14 / 0.22) * var20;
                }

                double var60 = Math::pow(var16 >= 1.0 ? 1.0 : var16, 0.45454545454545453) + var18;
                int var61 = (int)((var60 >= 1.0 ? 1.0 : var60) * 255.0 + 0.5);
                var0[((724067147 ^ 724066635) + var6) * (133436787 ^ 133435763) + var3 + var46] =
                    var61 * (1695566228 ^ 1678854293);
            }
        }
    }

    for (int var33 = 0; var33 < 32; var33++) {
        double var36 = ((double)var33 + 0.5 - 16.0) / 16.0;
        double var39 = Math::abs(var36);
        double var41 = (var39 - 0.8) * 5.0;
        var41 = var41 <= 0.0 ? 1.0 : (var41 >= 1.0 ? 0.0 : 1.0 - var41 * var41 * (3.0 - 2.0 * var41));

        for (int var8 = 0; var8 < (1053314140 ^ 1053314396); var8++) {
            double var47 = ((double)var8 + 0.5) / 256.0;
            double var50 = var36 / (0.18 + 0.25 * (1.0 - var47));
            double var13 = (var47 - 0.94) * 256.0;
            double var15 = var36 * 16.0;
            double var17 = Math::pow(var47, 2.2) * Math::exp(-var50 * var50) +
                           0.9 * Math::exp(-(var13 * var13 + var15 * var15) / 18.0);
            double var19 = (var47 - 0.97) * 33.333333333333336;
            var19 = var19 <= 0.0 ? 1.0 : (var19 >= 1.0 ? 0.0 : 1.0 - var19 * var19 * (3.0 - 2.0 * var19));
            var17 *= var19 * var41;
            int var21 = (int)((var17 >= 1.0 ? 1.0 : var17) * 255.0 + 0.5);
            var0[((-2029299421 ^ -2029298845) + var33) * (-122691151 ^ -122690127) + (-1596795114 ^ -1596795626) +
                 var8] = var21 * (1790759951 ^ 1807602958);
        }
    }

    double var34 = 110.08;

    for (int var37 = 0; var37 < (902947831 ^ 902947575); var37++) {
        double var40 = (double)var37 + 0.5 - 128.0;

        for (int var43 = 0; var43 < (506606587 ^ 506606331); var43++) {
            double var45 = (double)var43 + 0.5 - 128.0;
            double var48 = Math::sqrt(var45 * var45 + var40 * var40);
            double var51 = var34 - var48 + 0.5;
            var51 = var51 <= 0.0 ? 0.0 : (var51 >= 1.0 ? 1.0 : var51);
            double var53 = var48 / var34;
            double var54 = var53 >= 1.0 ? 0.0 : Math::sqrt(1.0 - var53 * var53);
            double var56 = var51 * (1.0 - 0.6 * (1.0 - var54));
            int var58 = (int)(var56 * 255.0 + 0.5);
            var0[((1882677559 ^ 1882678071) + var37) * (-701844346 ^ -701845370) + (1193097984 ^ 1193097216) + var43] =
                var58 * (-938567557 ^ -921724550);
        }
    }
}

Array<float> q(std::uint64_t var0) {

    Random var2 = Random(var0);
    Array<float> var3 = Array<float>(21600);

    for (int var4 = 0; var4 < (-2042930249 ^ -2042932521); var4++) {
        double var5;
        double var7;
        double var9;
        if (var4 < (241281638 ^ 241282894)) {
            double var11 = var2.nextDouble() * 2.0 - 1.0;
            double var13 = var2.nextDouble() * Math::PI * 2.0;
            double var15 = Math::sqrt(1.0 - var11 * var11);
            var5 = var15 * Math::cos(var13);
            var7 = var11;
            var9 = var15 * Math::sin(var13);
        } else {
            double var23 = var2.nextDouble() * Math::PI * 2.0;
            double var25 = var2.nextGaussian() * 0.22;
            double var28 = Math::cos(var25);
            double var17 = Math::sin(var25);
            double var19 = Math::cos(var23);
            double var21 = Math::sin(var23);
            var5 = var28 * (var19 * E1X + var21 * E2X) + var17 * NX;
            var7 = var28 * (var19 * E1Y + var21 * E2Y) + var17 * NY;
            var9 = var28 * (var19 * E1Z + var21 * E2Z) + var17 * NZ;
        }

        double var24 = var2.nextDouble();
        double var26 = 5.6 + Math::log10(Math::max(1.0E-9, var24)) / 0.45;
        var26 = var26 < -1.0 ? -1.0 : var26;
        double var29 = -0.4 * (var26 - 5.6);
        double var30 = (var29 - 1.0) / 1.6;
        var30 = var30 < 0.0 ? 0.0 : (var30 > 1.0 ? 1.0 : var30);
        int var32 = var4 * 9;
        var3[var32] = (float)var5;
        var3[var32 + 1] = (float)var7;
        var3[var32 + 2] = (float)var9;
        var3[var32 + 3] = (float)(1.5 + 1.1 * var30);
        var3[var32 + 4] = (float)Math::min(1.0, 0.17 * Math::pow(10.0, 0.42 * var29));
        var3[var32 + 5] = 0.3F;
        var3[var32 + 6] = (float)(7.0 + 9.0 * var2.nextDouble());
        var3[var32 + 7] = (float)((Math::PI * 2) * var2.nextDouble());
        var3[var32 + 8] = var26 < 0.6 ? 2.0F : (var26 < 2.6 ? 1.0F : 0.0F);
    }

    return var3;
}

Array<int> r(std::uint64_t var0) {

    Random var2 = Random(var0 ^ 25214903917L);
    Array<int> var3 = Array<int>(2400);

    for (int var4 = 0; var4 < (988075515 ^ 988077211); var4++) {
        double var5 = var2.nextDouble();
        var3[var4] = var5 < 0.3
                         ? -1032068093 ^ -1029566980
                         : (var5 < 0.7 ? 605438766 ^ 619296984 : (var5 < 0.9 ? 1775523504 ^ 1764472931 : 16767422));
    }

    return var3;
}

const bool initialized = []() {
    for (int var0 = 0; var0 < ad.size(); var0++) {
        int var1 = var0 * -1640531527 + 2135587861;
        var1 ^= (static_cast<std::uint32_t>(var1) >> 16);
        var1 *= -2048144789;
        var1 ^= (static_cast<std::uint32_t>(var1) >> 13);
        var1 *= -1028477387;
        var1 ^= (static_cast<std::uint32_t>(var1) >> 16);
        ad[var0] = (float)(var1 & 65535) / 65535.0F;
    }

    for (int var44 = 0; var44 <= 4096; var44++) {
        ae[var44] = (float)(255.0 * Math::pow((double)var44 / 4096.0, 0.9090909090909091));
    }

    for (int var45 = 0; var45 < 16; var45++) {
        af[var45] = af[var45] / 16.0;
    }

    double var46 = Math::sin(Math::toRadians(60.0)) * Math::cos(Math::toRadians(35.0));
    double var2 = 0.5;
    double var4 = Math::sin(Math::toRadians(60.0)) * Math::sin(Math::toRadians(35.0));
    NX = var46;
    NY = var2;
    NZ = var4;
    double var8 = -var46;
    double var10 = Math::sqrt(var4 * var4 + var8 * var8);
    double var6 = var4 / var10;
    var8 /= var10;
    double var12 = var2 * var8;
    double var14 = var4 * var6 - var46 * var8;
    double var16 = -var2 * var6;
    double var18 = Math::sqrt(var12 * var12 + var14 * var14 + var16 * var16);
    var12 /= var18;
    var14 /= var18;
    var16 /= var18;
    if (var14 < 0.0) {
        var12 = -var12;
        var14 = -var14;
        var16 = -var16;
    }

    double var20 = Math::sin(Math::toRadians(22.0)) / var14;
    double var22 = Math::sqrt(Math::max(0.0, 1.0 - var20 * var20));
    double var24 = var22 * var6 + var20 * var12;
    double var26 = var20 * var14;
    double var28 = var22 * var8 + var20 * var16;
    E1X = var24;
    E1Y = var26;
    E1Z = var28;
    E2X = var2 * var28 - var4 * var26;
    E2Y = var4 * var24 - var46 * var28;
    E2Z = var46 * var26 - var2 * var24;

    for (int var30 = 0; var30 < 3; var30++) {
        int var31 = var30 * 13;
        double var32 =
            Math::sqrt(NEB[var31] * NEB[var31] + NEB[var31 + 1] * NEB[var31 + 1] + NEB[var31 + 2] * NEB[var31 + 2]);
        NEB[var31] = NEB[var31] / var32;
        NEB[var31 + 1] = NEB[var31 + 1] / var32;
        NEB[var31 + 2] = NEB[var31 + 2] / var32;
        t[var30] = Math::cos(2.8 * NEB[var31 + 3]);
    }

    double var56 = -0.8;
    double var57 = 0.35;
    double var34 = 0.3;
    double var36 = Math::sqrt(var56 * var56 + var57 * var57 + var34 * var34);
    v = var56 / var36;
    w = var57 / var36;
    x = var34 / var36;
    double var38 = 0.26;
    double var40 = Math::sqrt(1.0 - var38 * var38);
    double var42 = Math::toRadians(-14.0);
    y = -Math::sin(var42) * var40;
    z = Math::cos(var42) * var40;
    aa = var38;
    return true;
}();
} // namespace vanta::modules::bloom::span
