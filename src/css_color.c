/* Nordstjernen — CSS Color 4/5 values: parsing, color spaces, color-mix(), relative colors and serialization.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include "css.h"
#include "css_color.h"

enum {
    CS_SRGB, CS_SRGB_LINEAR, CS_DISPLAY_P3, CS_DISPLAY_P3_LINEAR, CS_A98,
    CS_PROPHOTO, CS_REC2020, CS_XYZ_D50, CS_XYZ_D65, CS_LAB, CS_LCH,
    CS_OKLAB, CS_OKLCH, CS_HSL, CS_HWB, CS_COUNT
};

enum { AN_NONE, AN_RED, AN_GREEN, AN_BLUE, AN_LIGHT, AN_CHROMA, AN_HUE,
       AN_OPP_A, AN_OPP_B };

typedef struct {
    const char *name;
    int hue;
    guint8 analog[3];
    const char *channels[3];
} cs_info;

static const cs_info g_spaces[CS_COUNT] = {
    [CS_SRGB]              = { "srgb", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_SRGB_LINEAR]       = { "srgb-linear", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_DISPLAY_P3]        = { "display-p3", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_DISPLAY_P3_LINEAR] = { "display-p3-linear", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_A98]               = { "a98-rgb", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_PROPHOTO]          = { "prophoto-rgb", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_REC2020]           = { "rec2020", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "r", "g", "b" } },
    [CS_XYZ_D50]           = { "xyz-d50", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "x", "y", "z" } },
    [CS_XYZ_D65]           = { "xyz-d65", -1, { AN_RED, AN_GREEN, AN_BLUE }, { "x", "y", "z" } },
    [CS_LAB]               = { "lab", -1, { AN_LIGHT, AN_OPP_A, AN_OPP_B }, { "l", "a", "b" } },
    [CS_LCH]               = { "lch", 2, { AN_LIGHT, AN_CHROMA, AN_HUE }, { "l", "c", "h" } },
    [CS_OKLAB]             = { "oklab", -1, { AN_LIGHT, AN_OPP_A, AN_OPP_B }, { "l", "a", "b" } },
    [CS_OKLCH]             = { "oklch", 2, { AN_LIGHT, AN_CHROMA, AN_HUE }, { "l", "c", "h" } },
    [CS_HSL]               = { "hsl", 0, { AN_HUE, AN_CHROMA, AN_LIGHT }, { "h", "s", "l" } },
    [CS_HWB]               = { "hwb", 0, { AN_HUE, AN_NONE, AN_NONE }, { "h", "w", "b" } },
};

#define NONE_ALPHA 8u

typedef struct {
    int space;
    double c[3];
    double alpha;
    unsigned none;
    gboolean legacy;
} ns_color;

typedef enum { HUE_SHORTER, HUE_LONGER, HUE_INCREASING, HUE_DECREASING } hue_method;

static const char *const g_hue_names[] = { "shorter", "longer", "increasing", "decreasing" };

static void
mat3(const double m[9], const double in[3], double out[3])
{
    double x = in[0], y = in[1], z = in[2];
    out[0] = m[0] * x + m[1] * y + m[2] * z;
    out[1] = m[3] * x + m[4] * y + m[5] * z;
    out[2] = m[6] * x + m[7] * y + m[8] * z;
}

static const double M_SRGB_TO_XYZ[9] = {
    0.41239079926595934, 0.357584339383878, 0.1804807884018343,
    0.21263900587151027, 0.715168678767756, 0.07219231536073371,
    0.01933081871559182, 0.11919477979462598, 0.9505321522496607 };
static const double M_XYZ_TO_SRGB[9] = {
    3.2409699419045226, -1.537383177570094, -0.4986107602930034,
    -0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
    0.05563007969699366, -0.20397695888897652, 1.0569715142428786 };
static const double M_P3_TO_XYZ[9] = {
    0.4865709486482162, 0.26566769316909306, 0.1982172852343625,
    0.2289745640697488, 0.6917385218365064, 0.079286914093745,
    0.0, 0.04511338185890264, 1.043944368900976 };
static const double M_XYZ_TO_P3[9] = {
    2.493496911941425, -0.9313836179191239, -0.40271078445071684,
    -0.8294889695615747, 1.7626640603183463, 0.023624685841943577,
    0.03584583024378447, -0.07617238926804182, 0.9568845240076872 };
static const double M_A98_TO_XYZ[9] = {
    0.5766690429101305, 0.1855582379065463, 0.1882286462349947,
    0.29734497525053605, 0.6273635662554661, 0.07529145849399788,
    0.02703136138641234, 0.07068885253582723, 0.9913375368376388 };
static const double M_XYZ_TO_A98[9] = {
    2.0415879038107465, -0.5650069742788596, -0.34473135077832956,
    -0.9692436362808795, 1.8759675015077202, 0.04155505740717557,
    0.013444280632031142, -0.11836239223101838, 1.0151749943912054 };
static const double M_PROPHOTO_TO_XYZ50[9] = {
    0.7977604896723027, 0.13518583717574031, 0.0313493495815248,
    0.2880711282292934, 0.7118432178101014, 0.00008565396060525902,
    0.0, 0.0, 0.8251046025104601 };
static const double M_XYZ50_TO_PROPHOTO[9] = {
    1.3457989731028281, -0.25558010007997534, -0.05110628506753401,
    -0.5446224939028347, 1.5082327413132781, 0.02053603239147973,
    0.0, 0.0, 1.2119675456389454 };
static const double M_REC2020_TO_XYZ[9] = {
    0.6369580483012914, 0.14461690358620832, 0.1688809751641721,
    0.2627002120112671, 0.6779980715188708, 0.05930171646986196,
    0.0, 0.028072693049087428, 1.060985057710791 };
static const double M_XYZ_TO_REC2020[9] = {
    1.716651187971268, -0.355670783776392, -0.253366281373660,
    -0.666684351832489, 1.616481236634939, 0.0157685458139111,
    0.017639857445311, -0.042770613257809, 0.942103121235474 };
static const double M_D65_TO_D50[9] = {
    1.0479297925449969, 0.022946870601609652, -0.05019226628920524,
    0.02962780877005599, 0.9904344267538799, -0.017073799063418826,
    -0.009243040646204504, 0.015055191490298152, 0.7518742814281371 };
static const double M_D50_TO_D65[9] = {
    0.955473421488075, -0.02309845494876471, 0.06325924320057072,
    -0.0283697093338637, 1.0099953980813041, 0.021041441191917323,
    0.012314014864481998, -0.020507649298898964, 1.330365926242124 };
static const double M_XYZ_TO_LMS[9] = {
    0.8190224379967030, 0.3619062600528904, -0.1288737815209879,
    0.0329836539323885, 0.9292868615863434, 0.0361446663506424,
    0.0481771893596242, 0.2642395317527308, 0.6335478284694309 };
static const double M_LMS_TO_OKLAB[9] = {
    0.2104542683093140, 0.7936177747023054, -0.0040720430116193,
    1.9779985324311684, -2.4285922420485799, 0.4505937096174110,
    0.0259040424655478, 0.7827717124575296, -0.8086757549230774 };
static const double M_OKLAB_TO_LMS[9] = {
    1.0, 0.3963377773761749, 0.2158037573099136,
    1.0, -0.1055613458156586, -0.0638541728258133,
    1.0, -0.0894841775298119, -1.2914855480194092 };
static const double M_LMS_TO_XYZ[9] = {
    1.2268798758459243, -0.5578149944602171, 0.2813910456659647,
    -0.0405757452148008, 1.1122868032803170, -0.0717110580655164,
    -0.0763729366746601, -0.4214933324022432, 1.5869240198367816 };

static double
srgb_to_linear(double v)
{
    double a = fabs(v);
    if (a <= 0.04045) return v / 12.92;
    return copysign(pow((a + 0.055) / 1.055, 2.4), v);
}

static double
srgb_from_linear(double v)
{
    double a = fabs(v);
    if (a <= 0.0031308) return v * 12.92;
    return copysign(1.055 * pow(a, 1.0 / 2.4) - 0.055, v);
}

static double
a98_to_linear(double v)
{
    return copysign(pow(fabs(v), 563.0 / 256.0), v);
}

static double
a98_from_linear(double v)
{
    return copysign(pow(fabs(v), 256.0 / 563.0), v);
}

static double
prophoto_to_linear(double v)
{
    double a = fabs(v);
    if (a <= 16.0 / 512.0) return v / 16.0;
    return copysign(pow(a, 1.8), v);
}

static double
prophoto_from_linear(double v)
{
    double a = fabs(v);
    if (a >= 1.0 / 512.0) return copysign(pow(a, 1.0 / 1.8), v);
    return v * 16.0;
}

static double
rec2020_to_linear(double v)
{
    const double alpha = 1.09929682680944, beta = 0.018053968510807;
    double a = fabs(v);
    if (a < beta * 4.5) return v / 4.5;
    return copysign(pow((a + alpha - 1.0) / alpha, 1.0 / 0.45), v);
}

static double
rec2020_from_linear(double v)
{
    const double alpha = 1.09929682680944, beta = 0.018053968510807;
    double a = fabs(v);
    if (a > beta) return copysign(alpha * pow(a, 0.45) - (alpha - 1.0), v);
    return v * 4.5;
}

static const double D50_WHITE[3] = { 0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585 };

static void
xyz50_to_lab(const double xyz[3], double lab[3])
{
    const double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
    double f[3];
    for (int i = 0; i < 3; i++) {
        double v = xyz[i] / D50_WHITE[i];
        f[i] = v > eps ? cbrt(v) : (kappa * v + 16.0) / 116.0;
    }
    lab[0] = 116.0 * f[1] - 16.0;
    lab[1] = 500.0 * (f[0] - f[1]);
    lab[2] = 200.0 * (f[1] - f[2]);
}

static void
lab_to_xyz50(const double lab[3], double xyz[3])
{
    const double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
    double f1 = (lab[0] + 16.0) / 116.0;
    double f0 = lab[1] / 500.0 + f1;
    double f2 = f1 - lab[2] / 200.0;
    double x = f0 * f0 * f0 > eps ? f0 * f0 * f0 : (116.0 * f0 - 16.0) / kappa;
    double y = lab[0] > kappa * eps ? pow((lab[0] + 16.0) / 116.0, 3) : lab[0] / kappa;
    double z = f2 * f2 * f2 > eps ? f2 * f2 * f2 : (116.0 * f2 - 16.0) / kappa;
    xyz[0] = x * D50_WHITE[0];
    xyz[1] = y * D50_WHITE[1];
    xyz[2] = z * D50_WHITE[2];
}

static void
oklab_to_xyz65(const double lab[3], double xyz[3])
{
    double lms[3];
    mat3(M_OKLAB_TO_LMS, lab, lms);
    for (int i = 0; i < 3; i++) lms[i] = lms[i] * lms[i] * lms[i];
    mat3(M_LMS_TO_XYZ, lms, xyz);
}

static void
xyz65_to_oklab(const double xyz[3], double lab[3])
{
    double lms[3];
    mat3(M_XYZ_TO_LMS, xyz, lms);
    for (int i = 0; i < 3; i++) lms[i] = cbrt(lms[i]);
    mat3(M_LMS_TO_OKLAB, lms, lab);
}

static double
hue_norm(double h)
{
    if (!isfinite(h)) return 0.0;
    h = fmod(h, 360.0);
    if (h < 0) h += 360.0;
    if (h >= 360.0) h -= 360.0;
    return h;
}

static void
polar_to_rect(const double lch[3], double lab[3])
{
    double r = lch[2] * G_PI / 180.0;
    lab[0] = lch[0];
    lab[1] = lch[1] * cos(r);
    lab[2] = lch[1] * sin(r);
}

static void
rect_to_polar(const double lab[3], double lch[3])
{
    lch[0] = lab[0];
    lch[1] = hypot(lab[1], lab[2]);
    lch[2] = hue_norm(atan2(lab[2], lab[1]) * 180.0 / G_PI);
}

static double
hsl_channel(int n, double h, double s, double l)
{
    double k = fmod(n + h / 30.0, 12.0);
    double a = s * MIN(l, 1.0 - l);
    return l - a * MAX(-1.0, MIN(MIN(k - 3.0, 9.0 - k), 1.0));
}

static void
hsl_to_srgb(const double hsl[3], double rgb[3])
{
    double h = hue_norm(hsl[0]), s = hsl[1] / 100.0, l = hsl[2] / 100.0;
    rgb[0] = hsl_channel(0, h, s, l);
    rgb[1] = hsl_channel(8, h, s, l);
    rgb[2] = hsl_channel(4, h, s, l);
}

static void
srgb_to_hsl(const double rgb[3], double hsl[3])
{
    double mx = MAX(rgb[0], MAX(rgb[1], rgb[2]));
    double mn = MIN(rgb[0], MIN(rgb[1], rgb[2]));
    double h = 0.0, s = 0.0, l = (mn + mx) / 2.0, d = mx - mn;
    if (d != 0.0) {
        s = (l == 0.0 || l == 1.0) ? 0.0 : (mx - l) / MIN(l, 1.0 - l);
        if (mx == rgb[0])      h = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6.0 : 0.0);
        else if (mx == rgb[1]) h = (rgb[2] - rgb[0]) / d + 2.0;
        else                   h = (rgb[0] - rgb[1]) / d + 4.0;
        h *= 60.0;
    }
    if (s < 0) {
        h += 180.0;
        s = fabs(s);
    }
    hsl[0] = hue_norm(h);
    hsl[1] = s * 100.0;
    hsl[2] = l * 100.0;
}

static void
hwb_to_srgb(const double hwb[3], double rgb[3])
{
    double w = hwb[1] / 100.0, b = hwb[2] / 100.0;
    if (w + b >= 1.0) {
        double gray = w / (w + b);
        rgb[0] = rgb[1] = rgb[2] = gray;
        return;
    }
    double hsl[3] = { hwb[0], 100.0, 50.0 };
    hsl_to_srgb(hsl, rgb);
    for (int i = 0; i < 3; i++) rgb[i] = rgb[i] * (1.0 - w - b) + w;
}

static void
srgb_to_hwb(const double rgb[3], double hwb[3])
{
    double hsl[3];
    srgb_to_hsl(rgb, hsl);
    double w = MIN(rgb[0], MIN(rgb[1], rgb[2]));
    double b = 1.0 - MAX(rgb[0], MAX(rgb[1], rgb[2]));
    hwb[0] = hsl[0];
    hwb[1] = w * 100.0;
    hwb[2] = b * 100.0;
}

static void
to_xyz65(int space, const double c[3], double xyz[3])
{
    double v[3], t[3];
    switch (space) {
    case CS_SRGB:
        for (int i = 0; i < 3; i++) v[i] = srgb_to_linear(c[i]);
        mat3(M_SRGB_TO_XYZ, v, xyz);
        return;
    case CS_SRGB_LINEAR:
        mat3(M_SRGB_TO_XYZ, c, xyz);
        return;
    case CS_DISPLAY_P3:
        for (int i = 0; i < 3; i++) v[i] = srgb_to_linear(c[i]);
        mat3(M_P3_TO_XYZ, v, xyz);
        return;
    case CS_DISPLAY_P3_LINEAR:
        mat3(M_P3_TO_XYZ, c, xyz);
        return;
    case CS_A98:
        for (int i = 0; i < 3; i++) v[i] = a98_to_linear(c[i]);
        mat3(M_A98_TO_XYZ, v, xyz);
        return;
    case CS_PROPHOTO:
        for (int i = 0; i < 3; i++) v[i] = prophoto_to_linear(c[i]);
        mat3(M_PROPHOTO_TO_XYZ50, v, t);
        mat3(M_D50_TO_D65, t, xyz);
        return;
    case CS_REC2020:
        for (int i = 0; i < 3; i++) v[i] = rec2020_to_linear(c[i]);
        mat3(M_REC2020_TO_XYZ, v, xyz);
        return;
    case CS_XYZ_D50:
        mat3(M_D50_TO_D65, c, xyz);
        return;
    case CS_XYZ_D65:
        memcpy(xyz, c, sizeof(double) * 3);
        return;
    case CS_LAB:
        lab_to_xyz50(c, t);
        mat3(M_D50_TO_D65, t, xyz);
        return;
    case CS_LCH:
        polar_to_rect(c, v);
        lab_to_xyz50(v, t);
        mat3(M_D50_TO_D65, t, xyz);
        return;
    case CS_OKLAB:
        oklab_to_xyz65(c, xyz);
        return;
    case CS_OKLCH:
        polar_to_rect(c, v);
        oklab_to_xyz65(v, xyz);
        return;
    case CS_HSL:
        hsl_to_srgb(c, t);
        to_xyz65(CS_SRGB, t, xyz);
        return;
    case CS_HWB:
        hwb_to_srgb(c, t);
        to_xyz65(CS_SRGB, t, xyz);
        return;
    }
}

static void
from_xyz65(int space, const double xyz[3], double c[3])
{
    double v[3], t[3];
    switch (space) {
    case CS_SRGB:
        mat3(M_XYZ_TO_SRGB, xyz, v);
        for (int i = 0; i < 3; i++) c[i] = srgb_from_linear(v[i]);
        return;
    case CS_SRGB_LINEAR:
        mat3(M_XYZ_TO_SRGB, xyz, c);
        return;
    case CS_DISPLAY_P3:
        mat3(M_XYZ_TO_P3, xyz, v);
        for (int i = 0; i < 3; i++) c[i] = srgb_from_linear(v[i]);
        return;
    case CS_DISPLAY_P3_LINEAR:
        mat3(M_XYZ_TO_P3, xyz, c);
        return;
    case CS_A98:
        mat3(M_XYZ_TO_A98, xyz, v);
        for (int i = 0; i < 3; i++) c[i] = a98_from_linear(v[i]);
        return;
    case CS_PROPHOTO:
        mat3(M_D65_TO_D50, xyz, t);
        mat3(M_XYZ50_TO_PROPHOTO, t, v);
        for (int i = 0; i < 3; i++) c[i] = prophoto_from_linear(v[i]);
        return;
    case CS_REC2020:
        mat3(M_XYZ_TO_REC2020, xyz, v);
        for (int i = 0; i < 3; i++) c[i] = rec2020_from_linear(v[i]);
        return;
    case CS_XYZ_D50:
        mat3(M_D65_TO_D50, xyz, c);
        return;
    case CS_XYZ_D65:
        memcpy(c, xyz, sizeof(double) * 3);
        return;
    case CS_LAB:
        mat3(M_D65_TO_D50, xyz, t);
        xyz50_to_lab(t, c);
        return;
    case CS_LCH:
        mat3(M_D65_TO_D50, xyz, t);
        xyz50_to_lab(t, v);
        rect_to_polar(v, c);
        return;
    case CS_OKLAB:
        xyz65_to_oklab(xyz, c);
        return;
    case CS_OKLCH:
        xyz65_to_oklab(xyz, v);
        rect_to_polar(v, c);
        return;
    case CS_HSL:
        from_xyz65(CS_SRGB, xyz, t);
        srgb_to_hsl(t, c);
        return;
    case CS_HWB:
        from_xyz65(CS_SRGB, xyz, t);
        srgb_to_hwb(t, c);
        return;
    }
}

static gboolean
is_achromatic(int space, const double c[3])
{
    switch (space) {
    case CS_LCH:   return fabs(c[1]) <= 0.0015;
    case CS_OKLCH: return fabs(c[1]) <= 0.000004 + 1e-12;
    case CS_HSL:   return fabs(c[1]) <= 0.001 + 1e-9;
    case CS_HWB:   return c[1] + c[2] >= 100.0 - 0.001 - 1e-9;
    }
    return FALSE;
}

static int
analog_index(int space, int category)
{
    if (category == AN_NONE) return -1;
    for (int i = 0; i < 3; i++)
        if (g_spaces[space].analog[i] == category) return i;
    return -1;
}

static ns_color
color_convert(const ns_color *in, int space)
{
    if (in->space == space) return *in;
    ns_color out = { .space = space, .alpha = in->alpha,
                     .none = in->none & NONE_ALPHA };
    unsigned src_none = in->none;
    int src_hue = g_spaces[in->space].hue;
    if (src_hue >= 0 && is_achromatic(in->space, in->c)) src_none |= 1u << src_hue;
    double c[3], xyz[3];
    for (int i = 0; i < 3; i++) c[i] = (in->none & (1u << i)) ? 0.0 : in->c[i];
    if ((in->space == CS_HSL || in->space == CS_HWB) &&
        (space == CS_HSL || space == CS_HWB || space == CS_SRGB)) {
        double rgb[3];
        if (in->space == CS_HSL) hsl_to_srgb(c, rgb);
        else hwb_to_srgb(c, rgb);
        if (space == CS_SRGB) memcpy(out.c, rgb, sizeof rgb);
        else if (space == CS_HSL) srgb_to_hsl(rgb, out.c);
        else srgb_to_hwb(rgb, out.c);
    } else if (in->space == CS_SRGB && (space == CS_HSL || space == CS_HWB)) {
        if (space == CS_HSL) srgb_to_hsl(c, out.c);
        else srgb_to_hwb(c, out.c);
    } else if ((in->space == CS_LAB && space == CS_LCH) ||
               (in->space == CS_OKLAB && space == CS_OKLCH)) {
        rect_to_polar(c, out.c);
    } else if ((in->space == CS_LCH && space == CS_LAB) ||
               (in->space == CS_OKLCH && space == CS_OKLAB)) {
        polar_to_rect(c, out.c);
    } else {
        to_xyz65(in->space, c, xyz);
        from_xyz65(space, xyz, out.c);
    }
    for (int i = 0; i < 3; i++) {
        if (!(src_none & (1u << i))) continue;
        int j = analog_index(space, g_spaces[in->space].analog[i]);
        if (j >= 0) out.none |= 1u << j;
    }
    if ((in->none & 7u) == 7u) {
        out.none |= 7u;
    } else if ((in->space == CS_LAB || in->space == CS_OKLAB) &&
               (in->none & 6u) == 6u) {
        int j = analog_index(space, AN_CHROMA);
        if (j >= 0) out.none |= 1u << j;
    } else if (in->space == CS_HWB && (in->none & 6u) == 6u) {
        int j = analog_index(space, AN_CHROMA);
        int k = analog_index(space, AN_LIGHT);
        if (j >= 0) out.none |= 1u << j;
        if (k >= 0) out.none |= 1u << k;
    }
    int h = g_spaces[space].hue;
    if (h >= 0 && is_achromatic(space, out.c)) {
        out.none |= 1u << h;
        if (space == CS_HSL) out.c[1] = 0.0;
    }
    for (int i = 0; i < 3; i++)
        if (out.none & (1u << i)) out.c[i] = 0.0;
    return out;
}

static void
color_to_rgba8(const ns_color *col, guint8 rgba[4])
{
    ns_color s = color_convert(col, CS_SRGB);
    for (int i = 0; i < 3; i++) {
        double v = (s.none & (1u << i)) ? 0.0 : s.c[i];
        if (isnan(v)) v = 0.0;
        if (!isfinite(v)) v = v > 0 ? 1.0 : 0.0;
        rgba[i] = (guint8)CLAMP((int)floor(CLAMP(v, 0.0, 1.0) * 255.0 + 0.5 + 1e-7), 0, 255);
    }
    double a = (col->none & NONE_ALPHA) ? 0.0 : col->alpha;
    rgba[3] = (guint8)CLAMP((int)floor(CLAMP(a, 0.0, 1.0) * 255.0 + 0.5), 0, 255);
}

typedef enum {
    TK_EOF, TK_WS, TK_IDENT, TK_FUNC, TK_NUM, TK_HASH, TK_COMMA, TK_SLASH,
    TK_LPAREN, TK_RPAREN, TK_DELIM
} tk_type;

typedef struct {
    tk_type type;
    const char *start;
    int len;
    const char *name;
    int name_len;
    double num;
    char unit[12];
    char delim;
} tk;

typedef struct {
    tk *toks;
    int n;
    int pos;
} tks;

static gboolean
ident_start(unsigned char c)
{
    return g_ascii_isalpha(c) || c == '_' || c >= 0x80;
}

static gboolean
ident_char(unsigned char c)
{
    return ident_start(c) || g_ascii_isdigit(c) || c == '-';
}

static gboolean
css_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static gboolean
starts_number(const char *p)
{
    if (g_ascii_isdigit(p[0])) return TRUE;
    if (p[0] == '.' && g_ascii_isdigit(p[1])) return TRUE;
    if (p[0] == '+' || p[0] == '-') {
        if (g_ascii_isdigit(p[1])) return TRUE;
        if (p[1] == '.' && g_ascii_isdigit(p[2])) return TRUE;
    }
    return FALSE;
}

static gboolean
starts_ident(const char *p)
{
    if (p[0] == '-') return ident_start((unsigned char)p[1]) || p[1] == '-';
    return ident_start((unsigned char)p[0]);
}

static gboolean
tokenize(const char *s, GArray *out)
{
    const char *p = s;
    while (*p) {
        tk t = { .start = p };
        if (css_ws(*p)) {
            while (*p && css_ws(*p)) p++;
            t.type = TK_WS;
        } else if (starts_number(p)) {
            const char *q = p;
            if (*q == '+' || *q == '-') q++;
            while (g_ascii_isdigit(*q)) q++;
            if (*q == '.' && g_ascii_isdigit(q[1])) {
                q++;
                while (g_ascii_isdigit(*q)) q++;
            }
            if ((*q == 'e' || *q == 'E') &&
                (g_ascii_isdigit(q[1]) ||
                 ((q[1] == '+' || q[1] == '-') && g_ascii_isdigit(q[2])))) {
                q += 2;
                while (g_ascii_isdigit(*q)) q++;
            }
            char buf[64];
            gsize nl = MIN((gsize)(q - p), sizeof buf - 1);
            memcpy(buf, p, nl);
            buf[nl] = '\0';
            t.num = g_ascii_strtod(buf, NULL);
            t.type = TK_NUM;
            p = q;
            if (*p == '%') {
                strcpy(t.unit, "%");
                p++;
            } else if (starts_ident(p)) {
                int ul = 0;
                while (ident_char((unsigned char)*p)) {
                    if (ul < (int)sizeof t.unit - 1) t.unit[ul++] = g_ascii_tolower(*p);
                    p++;
                }
                t.unit[ul] = '\0';
            }
        } else if (starts_ident(p)) {
            t.name = p;
            while (ident_char((unsigned char)*p)) p++;
            t.name_len = (int)(p - t.name);
            if (*p == '(') {
                p++;
                t.type = TK_FUNC;
            } else {
                t.type = TK_IDENT;
            }
        } else if (*p == '#') {
            p++;
            t.name = p;
            while (ident_char((unsigned char)*p)) p++;
            t.name_len = (int)(p - t.name);
            t.type = TK_HASH;
        } else if (*p == ',') {
            p++;
            t.type = TK_COMMA;
        } else if (*p == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return FALSE;
            p = e + 2;
            continue;
        } else if (*p == '/') {
            p++;
            t.type = TK_SLASH;
        } else if (*p == '(') {
            p++;
            t.type = TK_LPAREN;
        } else if (*p == ')') {
            p++;
            t.type = TK_RPAREN;
        } else if (*p == '+' || *p == '-' || *p == '*') {
            t.delim = *p++;
            t.type = TK_DELIM;
        } else {
            return FALSE;
        }
        t.len = (int)(p - t.start);
        g_array_append_val(out, t);
    }
    tk eof = { .type = TK_EOF, .start = p };
    g_array_append_val(out, eof);
    return TRUE;
}

static tk *
peek(tks *ts)
{
    return &ts->toks[ts->pos];
}

static void
skip_ws(tks *ts)
{
    while (ts->toks[ts->pos].type == TK_WS) ts->pos++;
}

static tk *
next_nonws(tks *ts)
{
    skip_ws(ts);
    return peek(ts);
}

static gboolean
tk_is(const tk *t, tk_type type, const char *name)
{
    if (t->type != type) return FALSE;
    if (!name) return TRUE;
    return (int)strlen(name) == t->name_len &&
           g_ascii_strncasecmp(t->name, name, t->name_len) == 0;
}

static void
append_raw(GString *out, const tk *a, const tk *b)
{
    gboolean ws = FALSE;
    for (const tk *t = a; t < b; t++) {
        if (t->type == TK_WS) {
            ws = TRUE;
            continue;
        }
        if (ws && out->len && out->str[out->len - 1] != '(' && t->type != TK_RPAREN)
            g_string_append_c(out, ' ');
        ws = FALSE;
        if (t->type == TK_IDENT || t->type == TK_FUNC) {
            for (int i = 0; i < t->name_len; i++)
                g_string_append_c(out, g_ascii_tolower(t->name[i]));
            if (t->type == TK_FUNC) g_string_append_c(out, '(');
        } else {
            g_string_append_len(out, t->start, t->len);
        }
    }
}

enum { VT_NUM, VT_PCT, VT_ANGLE, VT_LEN };

typedef struct {
    double v;
    int type;
} cval;

typedef struct {
    const char *names[4];
    double values[4];
    gboolean none[4];
    int n;
} chan_env;

static gboolean calc_sum(tks *ts, const chan_env *env, cval *out, int depth);

static gboolean
unit_to_value(const tk *t, cval *out)
{
    const char *u = t->unit;
    out->v = t->num;
    if (!u[0])                     { out->type = VT_NUM; return TRUE; }
    if (strcmp(u, "%") == 0)       { out->type = VT_PCT; return TRUE; }
    out->type = VT_ANGLE;
    if (strcmp(u, "deg") == 0)  return TRUE;
    if (strcmp(u, "rad") == 0)  { out->v = t->num * 180.0 / G_PI; return TRUE; }
    if (strcmp(u, "grad") == 0) { out->v = t->num * 0.9; return TRUE; }
    if (strcmp(u, "turn") == 0) { out->v = t->num * 360.0; return TRUE; }
    out->type = VT_LEN;
    static const struct { const char *u; double px; } lens[] = {
        { "px", 1 }, { "em", 16 }, { "rem", 16 }, { "ex", 8 }, { "ch", 8 },
        { "vw", 8 }, { "vh", 6 }, { "vmin", 6 }, { "vmax", 8 }, { "cm", 96 / 2.54 },
        { "mm", 96 / 25.4 }, { "in", 96 }, { "pt", 96.0 / 72 }, { "pc", 16 },
        { "q", 96 / 101.6 }, { "cqw", 8 }, { "cqh", 6 }, { "lh", 19.2 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(lens); i++)
        if (strcmp(u, lens[i].u) == 0) {
            out->v = t->num * lens[i].px;
            return TRUE;
        }
    return FALSE;
}

static gboolean
calc_args(tks *ts, const chan_env *env, cval *args, int max, int *n, int depth)
{
    *n = 0;
    for (;;) {
        if (*n >= max) return FALSE;
        if (!calc_sum(ts, env, &args[*n], depth + 1)) return FALSE;
        (*n)++;
        tk *t = next_nonws(ts);
        if (t->type == TK_COMMA) {
            ts->pos++;
            continue;
        }
        if (t->type == TK_RPAREN) {
            ts->pos++;
            return TRUE;
        }
        return FALSE;
    }
}

static double
round_to(double v, double step, int mode)
{
    if (step == 0) return NAN;
    double q = v / step;
    switch (mode) {
    case 1: return ceil(q) * step;
    case 2: return floor(q) * step;
    case 3: return trunc(q) * step;
    default: return floor(q + 0.5) * step;
    }
}

static gboolean
calc_function(tks *ts, const tk *f, const chan_env *env, cval *out, int depth)
{
    cval a[16];
    int n = 0;
    char name[16];
    int nl = MIN(f->name_len, (int)sizeof name - 1);
    for (int i = 0; i < nl; i++) name[i] = g_ascii_tolower(f->name[i]);
    name[nl] = '\0';
    int round_mode = 0;
    if (strcmp(name, "round") == 0) {
        tk *t = next_nonws(ts);
        if (t->type == TK_IDENT) {
            static const char *modes[] = { "nearest", "up", "down", "to-zero" };
            for (int i = 0; i < 4; i++)
                if (tk_is(t, TK_IDENT, modes[i])) {
                    round_mode = i;
                    ts->pos++;
                    if (next_nonws(ts)->type != TK_COMMA) return FALSE;
                    ts->pos++;
                }
        }
    }
    if (!calc_args(ts, env, a, G_N_ELEMENTS(a), &n, depth)) return FALSE;
    if (strcmp(name, "calc") == 0) {
        if (n != 1) return FALSE;
        *out = a[0];
        return TRUE;
    }
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        *out = a[0];
        for (int i = 1; i < n; i++) {
            if (a[i].type != out->type) return FALSE;
            if (isnan(a[i].v) || isnan(out->v)) out->v = NAN;
            else if (name[1] == 'i' ? a[i].v < out->v : a[i].v > out->v) out->v = a[i].v;
        }
        return TRUE;
    }
    if (strcmp(name, "clamp") == 0) {
        if (n != 3 || a[0].type != a[1].type || a[1].type != a[2].type) return FALSE;
        *out = a[1];
        out->v = MAX(a[0].v, MIN(a[1].v, a[2].v));
        return TRUE;
    }
    if (strcmp(name, "abs") == 0 || strcmp(name, "sign") == 0) {
        if (n != 1) return FALSE;
        *out = a[0];
        if (name[0] == 'a') out->v = fabs(a[0].v);
        else {
            out->type = VT_NUM;
            out->v = a[0].v > 0 ? 1 : a[0].v < 0 ? -1 : a[0].v;
        }
        return TRUE;
    }
    if (strcmp(name, "sin") == 0 || strcmp(name, "cos") == 0 || strcmp(name, "tan") == 0) {
        if (n != 1 || (a[0].type != VT_NUM && a[0].type != VT_ANGLE)) return FALSE;
        double r = a[0].type == VT_ANGLE ? a[0].v * G_PI / 180.0 : a[0].v;
        out->type = VT_NUM;
        out->v = name[0] == 's' ? sin(r) : name[0] == 'c' ? cos(r) : tan(r);
        return TRUE;
    }
    if (strcmp(name, "asin") == 0 || strcmp(name, "acos") == 0 || strcmp(name, "atan") == 0) {
        if (n != 1 || a[0].type != VT_NUM) return FALSE;
        out->type = VT_ANGLE;
        double r = name[1] == 's' ? asin(a[0].v) : name[1] == 'c' ? acos(a[0].v) : atan(a[0].v);
        out->v = r * 180.0 / G_PI;
        return TRUE;
    }
    if (strcmp(name, "atan2") == 0) {
        if (n != 2 || a[0].type != a[1].type) return FALSE;
        out->type = VT_ANGLE;
        out->v = atan2(a[0].v, a[1].v) * 180.0 / G_PI;
        return TRUE;
    }
    if (strcmp(name, "pow") == 0) {
        if (n != 2 || a[0].type != VT_NUM || a[1].type != VT_NUM) return FALSE;
        out->type = VT_NUM;
        out->v = pow(a[0].v, a[1].v);
        return TRUE;
    }
    if (strcmp(name, "sqrt") == 0 || strcmp(name, "exp") == 0) {
        if (n != 1 || a[0].type != VT_NUM) return FALSE;
        out->type = VT_NUM;
        out->v = name[0] == 's' ? sqrt(a[0].v) : exp(a[0].v);
        return TRUE;
    }
    if (strcmp(name, "log") == 0) {
        if (n < 1 || n > 2 || a[0].type != VT_NUM) return FALSE;
        out->type = VT_NUM;
        out->v = n == 2 ? log(a[0].v) / log(a[1].v) : log(a[0].v);
        return TRUE;
    }
    if (strcmp(name, "hypot") == 0) {
        double s = 0;
        for (int i = 0; i < n; i++) {
            if (a[i].type != a[0].type) return FALSE;
            s += a[i].v * a[i].v;
        }
        out->type = a[0].type;
        out->v = sqrt(s);
        return TRUE;
    }
    if (strcmp(name, "mod") == 0 || strcmp(name, "rem") == 0) {
        if (n != 2 || a[0].type != a[1].type) return FALSE;
        *out = a[0];
        if (a[1].v == 0) out->v = NAN;
        else if (name[0] == 'r') out->v = fmod(a[0].v, a[1].v);
        else out->v = a[0].v - a[1].v * floor(a[0].v / a[1].v);
        return TRUE;
    }
    if (strcmp(name, "round") == 0) {
        if (n < 1 || n > 2) return FALSE;
        double step = 1;
        if (n == 2) {
            if (a[1].type != a[0].type) return FALSE;
            step = a[1].v;
        } else if (a[0].type != VT_NUM) {
            return FALSE;
        }
        *out = a[0];
        out->v = round_to(a[0].v, step, round_mode);
        return TRUE;
    }
    return FALSE;
}

static gboolean
calc_value(tks *ts, const chan_env *env, cval *out, int depth)
{
    if (depth > 32) return FALSE;
    tk *t = next_nonws(ts);
    if (t->type == TK_NUM) {
        ts->pos++;
        return unit_to_value(t, out);
    }
    if (t->type == TK_LPAREN) {
        ts->pos++;
        if (!calc_sum(ts, env, out, depth + 1)) return FALSE;
        if (next_nonws(ts)->type != TK_RPAREN) return FALSE;
        ts->pos++;
        return TRUE;
    }
    if (t->type == TK_FUNC) {
        ts->pos++;
        return calc_function(ts, t, env, out, depth + 1);
    }
    if (t->type == TK_IDENT) {
        ts->pos++;
        out->type = VT_NUM;
        if (tk_is(t, TK_IDENT, "e"))         { out->v = G_E; return TRUE; }
        if (tk_is(t, TK_IDENT, "pi"))        { out->v = G_PI; return TRUE; }
        if (tk_is(t, TK_IDENT, "infinity"))  { out->v = INFINITY; return TRUE; }
        if (tk_is(t, TK_IDENT, "-infinity")) { out->v = -INFINITY; return TRUE; }
        if (tk_is(t, TK_IDENT, "nan"))       { out->v = NAN; return TRUE; }
        for (int i = 0; env && i < env->n; i++)
            if (tk_is(t, TK_IDENT, env->names[i])) {
                out->v = env->none[i] ? 0.0 : env->values[i];
                return TRUE;
            }
        return FALSE;
    }
    return FALSE;
}

static gboolean
calc_product(tks *ts, const chan_env *env, cval *out, int depth)
{
    if (!calc_value(ts, env, out, depth)) return FALSE;
    for (;;) {
        int save = ts->pos;
        tk *t = next_nonws(ts);
        gboolean mul = t->type == TK_DELIM && t->delim == '*';
        gboolean div = t->type == TK_SLASH;
        if (!mul && !div) {
            ts->pos = save;
            return TRUE;
        }
        ts->pos++;
        cval rhs;
        if (!calc_value(ts, env, &rhs, depth)) return FALSE;
        if (mul) {
            if (out->type != VT_NUM && rhs.type != VT_NUM) return FALSE;
            if (out->type == VT_NUM) out->type = rhs.type;
            out->v *= rhs.v;
        } else {
            if (rhs.type != VT_NUM) return FALSE;
            out->v /= rhs.v;
        }
    }
}

static gboolean
calc_sum(tks *ts, const chan_env *env, cval *out, int depth)
{
    if (!calc_product(ts, env, out, depth)) return FALSE;
    for (;;) {
        int save = ts->pos;
        gboolean had_ws = peek(ts)->type == TK_WS;
        tk *t = next_nonws(ts);
        if (t->type != TK_DELIM || (t->delim != '+' && t->delim != '-')) {
            ts->pos = save;
            return TRUE;
        }
        if (!had_ws || ts->toks[ts->pos + 1].type != TK_WS) return FALSE;
        char op = t->delim;
        ts->pos++;
        cval rhs;
        if (!calc_product(ts, env, &rhs, depth)) return FALSE;
        if (rhs.type != out->type) return FALSE;
        out->v = op == '+' ? out->v + rhs.v : out->v - rhs.v;
    }
}

static gboolean
is_math_function(const tk *t)
{
    static const char *fns[] = {
        "calc", "min", "max", "clamp", "abs", "sign", "sin", "cos", "tan",
        "asin", "acos", "atan", "atan2", "pow", "sqrt", "exp", "log", "hypot",
        "mod", "rem", "round",
    };
    if (t->type != TK_FUNC) return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (tk_is(t, TK_FUNC, fns[i])) return TRUE;
    return FALSE;
}

typedef struct {
    double v;
    double raw;
    int type;
    gboolean none;
    gboolean keyword;
    gboolean math;
    int start, end;
} comp;

static gboolean
parse_comp(tks *ts, const chan_env *env, gboolean allow_none, comp *out)
{
    tk *t = next_nonws(ts);
    memset(out, 0, sizeof *out);
    out->start = ts->pos;
    if (t->type == TK_IDENT) {
        if (allow_none && tk_is(t, TK_IDENT, "none")) {
            ts->pos++;
            out->none = TRUE;
            out->end = ts->pos;
            return TRUE;
        }
        for (int i = 0; env && i < env->n; i++)
            if (tk_is(t, TK_IDENT, env->names[i])) {
                ts->pos++;
                out->type = VT_NUM;
                out->v = env->values[i];
                out->none = env->none[i];
                out->keyword = TRUE;
                out->end = ts->pos;
                return TRUE;
            }
        return FALSE;
    }
    cval cv;
    if (t->type == TK_NUM) {
        ts->pos++;
        if (!unit_to_value(t, &cv)) return FALSE;
    } else if (is_math_function(t)) {
        ts->pos++;
        if (!calc_function(ts, t, env, &cv, 0)) return FALSE;
        out->math = TRUE;
        out->raw = cv.v;
        if (isnan(cv.v)) cv.v = 0.0;
    } else {
        return FALSE;
    }
    out->v = cv.v;
    out->type = cv.type;
    out->end = ts->pos;
    return TRUE;
}

typedef struct {
    ns_color c;
    gboolean resolved;
    GString *spec;
} cres;

typedef struct {
    const guint8 *current;
    gboolean dark;
    int depth;
} pctx;

static gboolean parse_color_expr(tks *ts, pctx *cx, cres *out);

static void
fmt_num_bare(GString *s, double v)
{
    if (isnan(v)) {
        g_string_append(s, "NaN");
        return;
    }
    if (isinf(v)) {
        g_string_append(s, v > 0 ? "infinity" : "-infinity");
        return;
    }
    double a = fabs(v);
    int decimals = 6;
    if (a >= 1) {
        int digits = (int)floor(log10(a)) + 1;
        decimals = MAX(0, 7 - digits);
        if (decimals > 6) decimals = 6;
    }
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    char fmt[8];
    g_snprintf(fmt, sizeof fmt, "%%.%df", decimals);
    g_ascii_formatd(buf, sizeof buf, fmt, v);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *e = buf + strlen(buf) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (e == dot) *e = '\0';
    }
    if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
    g_string_append(s, buf);
}

static void
fmt_num(GString *s, double v)
{
    if (!isfinite(v)) {
        g_string_append(s, "calc(");
        fmt_num_bare(s, v);
        g_string_append_c(s, ')');
        return;
    }
    fmt_num_bare(s, v);
}

static int
round_channel(double v)
{
    if (!isfinite(v)) v = v > 0 ? 1.0 : 0.0;
    return (int)floor(CLAMP(v, 0.0, 1.0) * 255.0 + 0.5 + 1e-7);
}

static void
fmt_alpha8(GString *s, double alpha)
{
    guint8 a = (guint8)CLAMP((int)floor(CLAMP(alpha, 0.0, 1.0) * 255.0 + 0.5), 0, 255);
    char buf[16];
    double f = a / 255.0;
    for (int prec = 1; prec <= 5; prec++) {
        char fmt[8];
        g_snprintf(fmt, sizeof fmt, "%%.%df", prec);
        g_ascii_formatd(buf, sizeof buf, fmt, f);
        if ((int)(g_ascii_strtod(buf, NULL) * 255.0 + 0.5) == a) break;
    }
    char *dot = strchr(buf, '.');
    if (dot) {
        char *e = buf + strlen(buf) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (e == dot) *e = '\0';
    }
    g_string_append(s, buf);
}

static void
fmt_comp(GString *s, const ns_color *c, int i, gboolean pct)
{
    if (c->none & (1u << i)) {
        g_string_append(s, "none");
        return;
    }
    double v = c->c[i];
    if (g_spaces[c->space].hue == i) v = hue_norm(v);
    fmt_num(s, v);
    if (pct) g_string_append_c(s, '%');
}

static void
fmt_alpha_tail(GString *s, const ns_color *c)
{
    if (c->none & NONE_ALPHA) {
        g_string_append(s, " / none");
    } else if (c->alpha < 1.0) {
        g_string_append(s, " / ");
        fmt_num(s, CLAMP(c->alpha, 0.0, 1.0));
    }
}

static void
serialize_color(GString *s, const ns_color *in)
{
    ns_color c = *in;
    if ((c.space == CS_HSL || c.space == CS_HWB) && !c.none) {
        c = color_convert(in, CS_SRGB);
        c.legacy = in->legacy;
        c.none = in->none & NONE_ALPHA;
    }
    if (c.space == CS_SRGB && c.legacy && !c.none) {
        int v[3];
        for (int i = 0; i < 3; i++) v[i] = round_channel(c.c[i]);
        double a = CLAMP(c.alpha, 0.0, 1.0);
        if (a >= 1.0) {
            g_string_append_printf(s, "rgb(%d, %d, %d)", v[0], v[1], v[2]);
        } else {
            g_string_append_printf(s, "rgba(%d, %d, %d, ", v[0], v[1], v[2]);
            fmt_alpha8(s, a);
            g_string_append_c(s, ')');
        }
        return;
    }
    switch (c.space) {
    case CS_HSL:
    case CS_HWB:
        g_string_append(s, g_spaces[c.space].name);
        g_string_append_c(s, '(');
        fmt_comp(s, &c, 0, FALSE);
        g_string_append_c(s, ' ');
        fmt_comp(s, &c, 1, TRUE);
        g_string_append_c(s, ' ');
        fmt_comp(s, &c, 2, TRUE);
        break;
    case CS_LAB:
    case CS_LCH:
    case CS_OKLAB:
    case CS_OKLCH:
        g_string_append(s, g_spaces[c.space].name);
        g_string_append_c(s, '(');
        for (int i = 0; i < 3; i++) {
            if (i) g_string_append_c(s, ' ');
            fmt_comp(s, &c, i, FALSE);
        }
        break;
    default:
        g_string_append_printf(s, "color(%s", g_spaces[c.space].name);
        for (int i = 0; i < 3; i++) {
            g_string_append_c(s, ' ');
            fmt_comp(s, &c, i, FALSE);
        }
        break;
    }
    fmt_alpha_tail(s, &c);
    g_string_append_c(s, ')');
}

static void
clamp_color(ns_color *c)
{
    for (int i = 0; i < 3; i++)
        if (isnan(c->c[i])) c->c[i] = 0.0;
    if (isnan(c->alpha)) c->alpha = 0.0;
    c->alpha = CLAMP(c->alpha, 0.0, 1.0);
    switch (c->space) {
    case CS_LAB:
        c->c[0] = CLAMP(c->c[0], 0.0, 100.0);
        break;
    case CS_LCH:
        c->c[0] = CLAMP(c->c[0], 0.0, 100.0);
        c->c[1] = MAX(c->c[1], 0.0);
        break;
    case CS_OKLAB:
        c->c[0] = CLAMP(c->c[0], 0.0, 1.0);
        break;
    case CS_OKLCH:
        c->c[0] = CLAMP(c->c[0], 0.0, 1.0);
        c->c[1] = MAX(c->c[1], 0.0);
        break;
    }
    int h = g_spaces[c->space].hue;
    if (h >= 0 && !isfinite(c->c[h])) c->c[h] = 0.0;
}

static gboolean
needs_calc_form(const ns_color *c, const comp cps[4], gboolean alpha_given)
{
    if (c->space == CS_SRGB && c->legacy && !c->none) return FALSE;
    if ((c->space == CS_HSL || c->space == CS_HWB) && !c->none) return FALSE;
    for (int i = 0; i < 3; i++)
        if (cps[i].math) return TRUE;
    return alpha_given && cps[3].math;
}

static void
append_calc(GString *s, const comp *cp)
{
    g_string_append(s, "calc(");
    fmt_num_bare(s, cp->raw);
    if (cp->type == VT_PCT) g_string_append_c(s, '%');
    else if (cp->type == VT_ANGLE) g_string_append(s, "deg");
    g_string_append_c(s, ')');
}

static void
serialize_specified(GString *s, const ns_color *c, const comp cps[4],
                    gboolean alpha_given)
{
    if (!needs_calc_form(c, cps, alpha_given)) {
        serialize_color(s, c);
        return;
    }
    gboolean hsl_like = c->space == CS_HSL || c->space == CS_HWB;
    gboolean named_fn = hsl_like || c->space == CS_LAB || c->space == CS_LCH ||
                        c->space == CS_OKLAB || c->space == CS_OKLCH;
    if (named_fn) g_string_append_printf(s, "%s(", g_spaces[c->space].name);
    else g_string_append_printf(s, "color(%s ", g_spaces[c->space].name);
    for (int i = 0; i < 3; i++) {
        if (i) g_string_append_c(s, ' ');
        if (cps[i].math && !cps[i].none) append_calc(s, &cps[i]);
        else fmt_comp(s, c, i, hsl_like && i > 0);
    }
    if (alpha_given && cps[3].math && !cps[3].none) {
        g_string_append(s, " / ");
        append_calc(s, &cps[3]);
    } else {
        fmt_alpha_tail(s, c);
    }
    g_string_append_c(s, ')');
}

typedef struct {
    double pct_scale;
    gboolean hue;
} chan_rule;

static void
space_rules(int space, gboolean legacy_rgb, chan_rule r[3])
{
    for (int i = 0; i < 3; i++) {
        r[i].pct_scale = 0.01;
        r[i].hue = FALSE;
    }
    switch (space) {
    case CS_SRGB:
        if (legacy_rgb)
            for (int i = 0; i < 3; i++) r[i].pct_scale = 2.55;
        break;
    case CS_LAB:
        r[0].pct_scale = 1.0;
        r[1].pct_scale = r[2].pct_scale = 1.25;
        break;
    case CS_LCH:
        r[0].pct_scale = 1.0;
        r[1].pct_scale = 1.5;
        r[2].hue = TRUE;
        break;
    case CS_OKLAB:
        r[0].pct_scale = 0.01;
        r[1].pct_scale = r[2].pct_scale = 0.004;
        break;
    case CS_OKLCH:
        r[0].pct_scale = 0.01;
        r[1].pct_scale = 0.004;
        r[2].hue = TRUE;
        break;
    case CS_HSL:
    case CS_HWB:
        r[0].hue = TRUE;
        r[1].pct_scale = r[2].pct_scale = 1.0;
        break;
    }
}

static gboolean
comp_value(const comp *cp, const chan_rule *rule, gboolean modern_num_ok,
           double *out)
{
    if (rule->hue) {
        if (cp->type != VT_NUM && cp->type != VT_ANGLE) return FALSE;
        *out = cp->v;
        return TRUE;
    }
    if (cp->type == VT_PCT) {
        *out = cp->v * rule->pct_scale;
        return TRUE;
    }
    if (cp->type == VT_NUM && modern_num_ok) {
        *out = cp->v;
        return TRUE;
    }
    return FALSE;
}

static gboolean
alpha_value(const comp *cp, double *out)
{
    if (cp->type == VT_PCT) *out = cp->v / 100.0;
    else if (cp->type == VT_NUM) *out = cp->v;
    else return FALSE;
    return TRUE;
}

static int
space_by_name(const tk *t, gboolean allow_polar_and_hsl)
{
    if (t->type != TK_IDENT) return -1;
    if (tk_is(t, TK_IDENT, "xyz")) return CS_XYZ_D65;
    for (int i = 0; i < CS_COUNT; i++) {
        if (!allow_polar_and_hsl &&
            (i == CS_LAB || i == CS_LCH || i == CS_OKLAB || i == CS_OKLCH ||
             i == CS_HSL || i == CS_HWB))
            continue;
        if (tk_is(t, TK_IDENT, g_spaces[i].name)) return i;
    }
    return -1;
}

static gboolean
expect_close(tks *ts)
{
    if (next_nonws(ts)->type != TK_RPAREN) return FALSE;
    ts->pos++;
    return TRUE;
}

static void
env_from_color(chan_env *env, const ns_color *c, gboolean rgb255)
{
    env->n = 4;
    for (int i = 0; i < 3; i++) {
        env->names[i] = g_spaces[c->space].channels[i];
        env->values[i] = c->c[i] * (rgb255 ? 255.0 : 1.0);
        env->none[i] = (c->none & (1u << i)) != 0;
    }
    env->names[3] = "alpha";
    env->values[3] = c->alpha;
    env->none[3] = (c->none & NONE_ALPHA) != 0;
}

static gboolean
parse_channels(tks *ts, const chan_env *env, int space, gboolean rgb255,
               gboolean relative, gboolean legacy_ok, ns_color *out,
               GString *raw, gboolean *was_legacy, comp cps[4],
               gboolean *alpha_given)
{
    chan_rule rules[3];
    space_rules(space, rgb255, rules);
    gboolean have_alpha = FALSE;
    memset(cps, 0, sizeof(comp) * 4);
    *was_legacy = FALSE;
    if (!parse_comp(ts, env, TRUE, &cps[0])) return FALSE;
    tk *t = next_nonws(ts);
    if (t->type == TK_COMMA) {
        if (!legacy_ok || relative || cps[0].none) return FALSE;
        *was_legacy = TRUE;
        for (int i = 1; i < 3; i++) {
            if (next_nonws(ts)->type != TK_COMMA) return FALSE;
            ts->pos++;
            if (!parse_comp(ts, env, FALSE, &cps[i])) return FALSE;
        }
        t = next_nonws(ts);
        if (t->type == TK_COMMA) {
            ts->pos++;
            if (!parse_comp(ts, env, FALSE, &cps[3])) return FALSE;
            have_alpha = TRUE;
        }
        if (space == CS_SRGB) {
            gboolean pct = cps[0].type == VT_PCT;
            for (int i = 0; i < 3; i++)
                if ((cps[i].type == VT_PCT) != pct ||
                    (cps[i].type != VT_PCT && cps[i].type != VT_NUM))
                    return FALSE;
        } else if (space == CS_HSL) {
            if (cps[0].type != VT_NUM && cps[0].type != VT_ANGLE) return FALSE;
            if (cps[1].type != VT_PCT || cps[2].type != VT_PCT) return FALSE;
        }
    } else {
        for (int i = 1; i < 3; i++)
            if (!parse_comp(ts, env, TRUE, &cps[i])) return FALSE;
        t = next_nonws(ts);
        if (t->type == TK_SLASH) {
            ts->pos++;
            if (!parse_comp(ts, env, TRUE, &cps[3])) return FALSE;
            have_alpha = TRUE;
        }
    }
    if (!expect_close(ts)) return FALSE;
    memset(out, 0, sizeof *out);
    out->space = space;
    out->alpha = 1.0;
    for (int i = 0; i < 3; i++) {
        if (cps[i].none) {
            out->none |= 1u << i;
            continue;
        }
        double v;
        if (!comp_value(&cps[i], &rules[i], TRUE, &v)) return FALSE;
        if (rgb255) v /= 255.0;
        out->c[i] = v;
    }
    if (have_alpha) {
        if (cps[3].none) {
            out->none |= NONE_ALPHA;
        } else if (!alpha_value(&cps[3], &out->alpha)) {
            return FALSE;
        }
    } else if (relative) {
        out->alpha = env->values[3];
        if (env->none[3]) out->none |= NONE_ALPHA;
    }
    *alpha_given = have_alpha;
    if (raw) {
        for (int i = 0; i < 3; i++) {
            g_string_append_c(raw, ' ');
            append_raw(raw, &ts->toks[cps[i].start], &ts->toks[cps[i].end]);
        }
        if (have_alpha) {
            g_string_append(raw, " / ");
            append_raw(raw, &ts->toks[cps[3].start], &ts->toks[cps[3].end]);
        }
    }
    return TRUE;
}

static gboolean
parse_origin(tks *ts, pctx *cx, cres *origin)
{
    tk *t = next_nonws(ts);
    if (!tk_is(t, TK_IDENT, "from")) return FALSE;
    ts->pos++;
    skip_ws(ts);
    return parse_color_expr(ts, cx, origin);
}

static gboolean
parse_space_fn(tks *ts, pctx *cx, int space, gboolean legacy_ok,
               const char *fname, cres *out)
{
    tk *t = next_nonws(ts);
    gboolean rgb255 = space == CS_SRGB;
    if (tk_is(t, TK_IDENT, "from")) {
        cres origin = { 0 };
        if (!parse_origin(ts, cx, &origin)) {
            if (origin.spec) g_string_free(origin.spec, TRUE);
            return FALSE;
        }
        ns_color oc = { 0 };
        chan_env env = { 0 };
        if (origin.resolved) {
            oc = color_convert(&origin.c, space);
            env_from_color(&env, &oc, rgb255);
        } else {
            oc.space = space;
            oc.alpha = 1.0;
            env_from_color(&env, &oc, rgb255);
        }
        GString *raw = g_string_new(NULL);
        ns_color res;
        gboolean was_legacy, alpha_given;
        comp cps[4];
        gboolean ok = parse_channels(ts, &env, space, rgb255, TRUE, FALSE, &res,
                                     raw, &was_legacy, cps, &alpha_given);
        if (ok) {
            clamp_color(&res);
            out->c = res;
            out->resolved = origin.resolved;
            out->spec = g_string_new(fname);
            g_string_append(out->spec, "(from ");
            g_string_append(out->spec, origin.spec->str);
            g_string_append(out->spec, raw->str);
            g_string_append_c(out->spec, ')');
        }
        g_string_free(raw, TRUE);
        g_string_free(origin.spec, TRUE);
        return ok;
    }
    ns_color res;
    gboolean was_legacy, alpha_given;
    comp cps[4];
    if (!parse_channels(ts, NULL, space, rgb255, FALSE, legacy_ok, &res, NULL,
                        &was_legacy, cps, &alpha_given))
        return FALSE;
    for (int i = 0; i < 3; i++) {
        if (res.none & (1u << i)) continue;
        if (isnan(res.c[i])) res.c[i] = 0;
        if (space == CS_SRGB) res.c[i] = CLAMP(res.c[i], 0.0, 1.0);
        else if (space == CS_HSL && i == 1) res.c[i] = MAX(res.c[i], 0.0);
    }
    clamp_color(&res);
    res.legacy = space == CS_SRGB || space == CS_HSL || space == CS_HWB;
    out->c = res;
    out->resolved = TRUE;
    out->spec = g_string_new(NULL);
    serialize_specified(out->spec, &res, cps, alpha_given);
    return TRUE;
}

static gboolean
parse_color_fn(tks *ts, pctx *cx, cres *out)
{
    tk *t = next_nonws(ts);
    cres origin = { 0 };
    gboolean relative = FALSE;
    if (tk_is(t, TK_IDENT, "from")) {
        relative = TRUE;
        if (!parse_origin(ts, cx, &origin)) {
            if (origin.spec) g_string_free(origin.spec, TRUE);
            return FALSE;
        }
    }
    t = next_nonws(ts);
    int space = space_by_name(t, FALSE);
    if (space < 0) {
        if (origin.spec) g_string_free(origin.spec, TRUE);
        return FALSE;
    }
    ts->pos++;
    chan_env env = { 0 };
    ns_color oc = { .space = space, .alpha = 1.0 };
    if (relative && origin.resolved) oc = color_convert(&origin.c, space);
    env_from_color(&env, &oc, FALSE);
    GString *raw = relative ? g_string_new(NULL) : NULL;
    ns_color res;
    gboolean was_legacy, alpha_given;
    comp cps[4];
    gboolean ok = parse_channels(ts, relative ? &env : NULL, space, FALSE,
                                 relative, FALSE, &res, raw, &was_legacy, cps,
                                 &alpha_given);
    if (ok) {
        clamp_color(&res);
        out->c = res;
        out->resolved = relative ? origin.resolved : TRUE;
        out->spec = g_string_new(NULL);
        if (relative) {
            g_string_append(out->spec, "color(from ");
            g_string_append(out->spec, origin.spec->str);
            g_string_append_c(out->spec, ' ');
            g_string_append(out->spec, g_spaces[space].name);
            g_string_append(out->spec, raw->str);
            g_string_append_c(out->spec, ')');
        } else {
            serialize_specified(out->spec, &res, cps, alpha_given);
        }
    }
    if (raw) g_string_free(raw, TRUE);
    if (origin.spec) g_string_free(origin.spec, TRUE);
    return ok;
}

static void
fixup_hues(double *a, double *b, hue_method m)
{
    double d = *b - *a;
    switch (m) {
    case HUE_SHORTER:
        if (d > 180) *a += 360;
        else if (d < -180) *b += 360;
        break;
    case HUE_LONGER:
        if (d > 0 && d < 180) *a += 360;
        else if (d > -180 && d <= 0) *b += 360;
        break;
    case HUE_INCREASING:
        if (d < 0) *b += 360;
        break;
    case HUE_DECREASING:
        if (d > 0) *a += 360;
        break;
    }
}

static ns_color
interpolate(const ns_color *x, const ns_color *y, double t, int space,
            hue_method method)
{
    ns_color a = color_convert(x, space);
    ns_color b = color_convert(y, space);
    ns_color r = { .space = space };
    int hue = g_spaces[space].hue;
    for (int i = 0; i < 4; i++) {
        unsigned bit = i == 3 ? NONE_ALPHA : 1u << i;
        double *av = i == 3 ? &a.alpha : &a.c[i];
        double *bv = i == 3 ? &b.alpha : &b.c[i];
        if ((a.none & bit) && (b.none & bit)) {
            r.none |= bit;
            *av = *bv = i == 3 ? 1.0 : 0.0;
        } else if (a.none & bit) {
            *av = *bv;
        } else if (b.none & bit) {
            *bv = *av;
        }
    }
    double aa = a.alpha, ba = b.alpha;
    double ra = aa * (1 - t) + ba * t;
    for (int i = 0; i < 3; i++) {
        if (r.none & (1u << i)) continue;
        if (i == hue) {
            double h1 = hue_norm(a.c[i]), h2 = hue_norm(b.c[i]);
            fixup_hues(&h1, &h2, method);
            r.c[i] = hue_norm(h1 * (1 - t) + h2 * t);
            continue;
        }
        double v = a.c[i] * aa * (1 - t) + b.c[i] * ba * t;
        r.c[i] = ra != 0 ? v / ra : v;
    }
    r.alpha = (r.none & NONE_ALPHA) ? 0.0 : ra;
    return r;
}

typedef struct {
    cres color;
    gboolean has_pct;
    gboolean pct_literal;
    double pct;
    GString *pct_raw;
} mix_item;

static void
mix_items_free(GArray *items)
{
    for (guint i = 0; i < items->len; i++) {
        mix_item *m = &g_array_index(items, mix_item, i);
        if (m->color.spec) g_string_free(m->color.spec, TRUE);
        if (m->pct_raw) g_string_free(m->pct_raw, TRUE);
    }
    g_array_free(items, TRUE);
}

static gboolean
parse_mix_pct(tks *ts, mix_item *m)
{
    tk *t = next_nonws(ts);
    int start = ts->pos;
    cval cv;
    if (t->type == TK_NUM) {
        if (strcmp(t->unit, "%") != 0) return FALSE;
        ts->pos++;
        m->pct = t->num;
        m->pct_literal = TRUE;
        if (m->pct < 0 || m->pct > 100) return FALSE;
    } else if (is_math_function(t)) {
        ts->pos++;
        if (!calc_function(ts, t, NULL, &cv, 0) || cv.type != VT_PCT) return FALSE;
        m->pct = isnan(cv.v) ? 0 : CLAMP(cv.v, 0.0, 100.0);
    } else {
        return FALSE;
    }
    m->has_pct = TRUE;
    m->pct_raw = g_string_new(NULL);
    append_raw(m->pct_raw, &ts->toks[start], &ts->toks[ts->pos]);
    return TRUE;
}

static gboolean
pct_follows(tks *ts)
{
    tk *t = next_nonws(ts);
    return (t->type == TK_NUM && strcmp(t->unit, "%") == 0) || is_math_function(t);
}

static gboolean
parse_color_mix(tks *ts, pctx *cx, cres *out)
{
    int space = CS_OKLAB;
    hue_method method = HUE_SHORTER;
    gboolean has_in = FALSE;
    const char *space_word = NULL;
    tk *t = next_nonws(ts);
    if (tk_is(t, TK_IDENT, "in")) {
        has_in = TRUE;
        ts->pos++;
        t = next_nonws(ts);
        space = space_by_name(t, TRUE);
        if (space < 0) return FALSE;
        space_word = tk_is(t, TK_IDENT, "xyz") ? "xyz" : g_spaces[space].name;
        ts->pos++;
        t = next_nonws(ts);
        if (t->type == TK_IDENT) {
            if (g_spaces[space].hue < 0) return FALSE;
            int found = -1;
            for (int i = 0; i < 4; i++)
                if (tk_is(t, TK_IDENT, g_hue_names[i])) found = i;
            if (found < 0) return FALSE;
            ts->pos++;
            if (!tk_is(next_nonws(ts), TK_IDENT, "hue")) return FALSE;
            ts->pos++;
            method = (hue_method)found;
        }
        if (next_nonws(ts)->type != TK_COMMA) return FALSE;
        ts->pos++;
    }
    GArray *items = g_array_new(FALSE, TRUE, sizeof(mix_item));
    gboolean ok = TRUE;
    for (;;) {
        mix_item m = { 0 };
        if (pct_follows(ts) && !parse_mix_pct(ts, &m)) ok = FALSE;
        if (ok) {
            skip_ws(ts);
            ok = parse_color_expr(ts, cx, &m.color);
        }
        if (ok && !m.has_pct && pct_follows(ts)) ok = parse_mix_pct(ts, &m);
        g_array_append_val(items, m);
        if (!ok) break;
        t = next_nonws(ts);
        if (t->type == TK_COMMA) {
            ts->pos++;
            continue;
        }
        if (t->type == TK_RPAREN) {
            ts->pos++;
            break;
        }
        ok = FALSE;
        break;
    }
    if (!ok || items->len < 1) {
        mix_items_free(items);
        return FALSE;
    }
    int n = (int)items->len;
    mix_item *it = &g_array_index(items, mix_item, 0);
    if (n == 2) {
        if (it[0].has_pct && it[0].pct_literal && !it[1].has_pct) {
            it[1].has_pct = TRUE;
            it[1].pct_literal = TRUE;
            it[1].pct = 100.0 - it[0].pct;
            it[1].pct_raw = g_string_new(NULL);
            fmt_num(it[1].pct_raw, it[1].pct);
            g_string_append_c(it[1].pct_raw, '%');
        } else if (it[1].has_pct && it[1].pct_literal && !it[0].has_pct) {
            it[0].has_pct = TRUE;
            it[0].pct_literal = TRUE;
            it[0].pct = 100.0 - it[1].pct;
            it[0].pct_raw = g_string_new(NULL);
            fmt_num(it[0].pct_raw, it[0].pct);
            g_string_append_c(it[0].pct_raw, '%');
        }
    }
    gboolean omit_pcts = n == 2 && it[0].has_pct && it[1].has_pct &&
                         it[0].pct_literal && it[1].pct_literal &&
                         it[0].pct == 50.0 && it[1].pct == 50.0;
    if (n == 1 && it[0].has_pct && it[0].pct_literal && it[0].pct == 100.0)
        omit_pcts = TRUE;
    out->spec = g_string_new("color-mix(");
    if (has_in && !(space == CS_OKLAB && method == HUE_SHORTER)) {
        g_string_append_printf(out->spec, "in %s", space_word);
        if (method != HUE_SHORTER)
            g_string_append_printf(out->spec, " %s hue", g_hue_names[method]);
        g_string_append(out->spec, ", ");
    }
    gboolean resolved = TRUE;
    for (int i = 0; i < n; i++) {
        if (i) g_string_append(out->spec, ", ");
        g_string_append(out->spec, it[i].color.spec->str);
        if (it[i].has_pct && !omit_pcts) {
            g_string_append_c(out->spec, ' ');
            if (it[i].pct_literal) {
                fmt_num(out->spec, it[i].pct);
                g_string_append_c(out->spec, '%');
            } else {
                g_string_append(out->spec, it[i].pct_raw->str);
            }
        }
        if (!it[i].color.resolved) resolved = FALSE;
    }
    g_string_append_c(out->spec, ')');
    out->resolved = resolved;
    if (resolved) {
        double given = 0;
        int unspecified = 0;
        for (int i = 0; i < n; i++) {
            if (it[i].has_pct) given += it[i].pct;
            else unspecified++;
        }
        double w[64];
        double fill = unspecified ? MAX(0.0, (100.0 - given) / unspecified) : 0;
        double total = 0;
        int m = MIN(n, (int)G_N_ELEMENTS(w));
        for (int i = 0; i < m; i++) {
            w[i] = it[i].has_pct ? it[i].pct : fill;
            total += w[i];
        }
        double mult = 1.0;
        if (total <= 0) {
            for (int i = 0; i < m; i++) w[i] = 1.0;
            total = m;
            mult = 0.0;
        } else if (total < 100.0) {
            mult = total / 100.0;
        }
        ns_color acc = color_convert(&it[0].color.c, space);
        double wacc = w[0];
        for (int i = 1; i < m; i++) {
            double sum = wacc + w[i];
            double tt = sum > 0 ? w[i] / sum : 0.5;
            acc = interpolate(&acc, &it[i].color.c, tt, space, method);
            wacc = sum;
        }
        if (!(acc.none & NONE_ALPHA)) acc.alpha *= mult;
        acc.legacy = FALSE;
        if ((acc.space == CS_HSL || acc.space == CS_HWB) && !acc.none) {
            ns_color s = color_convert(&acc, CS_SRGB);
            s.none = acc.none & NONE_ALPHA;
            acc = s;
        }
        clamp_color(&acc);
        out->c = acc;
    }
    mix_items_free(items);
    return TRUE;
}

static gboolean
parse_alpha_fn(tks *ts, pctx *cx, cres *out)
{
    cres origin = { 0 };
    if (!parse_origin(ts, cx, &origin)) {
        if (origin.spec) g_string_free(origin.spec, TRUE);
        return FALSE;
    }
    ns_color res = origin.c;
    if (res.legacy) {
        res = color_convert(&origin.c, CS_SRGB);
        res.legacy = FALSE;
    }
    chan_env env = { .n = 1, .names = { "alpha" }, .values = { origin.c.alpha },
                     .none = { (origin.c.none & NONE_ALPHA) != 0 } };
    gboolean ok = TRUE;
    comp a = { 0 };
    gboolean have_alpha = FALSE;
    tk *t = next_nonws(ts);
    if (t->type != TK_SLASH) ok = FALSE;
    if (ok) {
        ts->pos++;
        ok = parse_comp(ts, &env, TRUE, &a);
        have_alpha = ok;
        if (ok) {
            res.none &= ~NONE_ALPHA;
            if (a.none) res.none |= NONE_ALPHA;
            else ok = alpha_value(&a, &res.alpha);
        }
    }
    if (ok) ok = expect_close(ts);
    if (ok) {
        clamp_color(&res);
        out->c = res;
        out->resolved = origin.resolved;
        out->spec = g_string_new("alpha(from ");
        g_string_append(out->spec, origin.spec->str);
        if (have_alpha) {
            g_string_append(out->spec, " / ");
            append_raw(out->spec, &ts->toks[a.start], &ts->toks[a.end]);
        }
        g_string_append_c(out->spec, ')');
    }
    g_string_free(origin.spec, TRUE);
    return ok;
}

static double
relative_luminance(const ns_color *c)
{
    ns_color s = color_convert(c, CS_SRGB_LINEAR);
    double v[3];
    for (int i = 0; i < 3; i++) v[i] = CLAMP(s.c[i], 0.0, 1.0);
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}

static gboolean
parse_contrast_color(tks *ts, pctx *cx, cres *out)
{
    cres base = { 0 };
    skip_ws(ts);
    gboolean ok = parse_color_expr(ts, cx, &base) && expect_close(ts);
    if (ok) {
        out->spec = g_string_new("contrast-color(");
        g_string_append(out->spec, base.spec->str);
        g_string_append_c(out->spec, ')');
        out->resolved = base.resolved;
        double l = relative_luminance(&base.c);
        double v = (1.05 / (l + 0.05)) > ((l + 0.05) / 0.05) ? 1.0 : 0.0;
        ns_color r = { .space = CS_SRGB, .c = { v, v, v }, .alpha = 1.0, .legacy = TRUE };
        out->c = r;
    }
    if (base.spec) g_string_free(base.spec, TRUE);
    return ok;
}

static gboolean
parse_color_layers(tks *ts, pctx *cx, cres *out)
{
    static const char *const modes[] = {
        "normal", "multiply", "screen", "overlay", "darken", "lighten",
        "color-dodge", "color-burn", "hard-light", "soft-light", "difference",
        "exclusion", "hue", "saturation", "color", "luminosity",
    };
    tk *t = next_nonws(ts);
    int mode = -1;
    if (t->type == TK_IDENT) {
        for (gsize i = 0; i < G_N_ELEMENTS(modes); i++)
            if (tk_is(t, TK_IDENT, modes[i])) mode = (int)i;
        if (mode >= 0 && ts->toks[ts->pos + 1].type != TK_COMMA &&
            !(ts->toks[ts->pos + 1].type == TK_WS && ts->toks[ts->pos + 2].type == TK_COMMA))
            mode = -1;
        if (mode >= 0) {
            ts->pos++;
            next_nonws(ts);
            ts->pos++;
        }
    }
    out->spec = g_string_new("color-layers(");
    if (mode > 0) g_string_append_printf(out->spec, "%s, ", modes[mode]);
    out->resolved = TRUE;
    ns_color acc = { .space = CS_SRGB, .alpha = 0.0 };
    int n = 0;
    for (;;) {
        cres layer = { 0 };
        skip_ws(ts);
        if (!parse_color_expr(ts, cx, &layer)) {
            g_string_free(out->spec, TRUE);
            out->spec = NULL;
            return FALSE;
        }
        if (n++) g_string_append(out->spec, ", ");
        g_string_append(out->spec, layer.spec->str);
        g_string_free(layer.spec, TRUE);
        if (!layer.resolved) out->resolved = FALSE;
        ns_color top = color_convert(&layer.c, CS_SRGB);
        double ta = (top.none & NONE_ALPHA) ? 0.0 : top.alpha;
        double ra = ta + acc.alpha * (1.0 - ta);
        for (int i = 0; i < 3; i++) {
            double tc = (top.none & (1u << i)) ? 0.0 : top.c[i];
            double v = tc * ta + acc.c[i] * acc.alpha * (1.0 - ta);
            acc.c[i] = ra > 0 ? v / ra : 0.0;
        }
        acc.alpha = ra;
        t = next_nonws(ts);
        if (t->type == TK_COMMA) {
            ts->pos++;
            continue;
        }
        if (t->type == TK_RPAREN) {
            ts->pos++;
            break;
        }
        g_string_free(out->spec, TRUE);
        out->spec = NULL;
        return FALSE;
    }
    g_string_append_c(out->spec, ')');
    out->c = acc;
    return TRUE;
}

static gboolean
parse_light_dark(tks *ts, pctx *cx, cres *out)
{
    cres a = { 0 }, b = { 0 };
    skip_ws(ts);
    gboolean ok = parse_color_expr(ts, cx, &a);
    if (ok) {
        ok = next_nonws(ts)->type == TK_COMMA;
        ts->pos++;
        skip_ws(ts);
    }
    if (ok) ok = parse_color_expr(ts, cx, &b) && expect_close(ts);
    if (ok) {
        out->spec = g_string_new("light-dark(");
        g_string_append(out->spec, a.spec->str);
        g_string_append(out->spec, ", ");
        g_string_append(out->spec, b.spec->str);
        g_string_append_c(out->spec, ')');
        cres *pick = cx->dark ? &b : &a;
        out->c = pick->c;
        out->resolved = pick->resolved;
    }
    if (a.spec) g_string_free(a.spec, TRUE);
    if (b.spec) g_string_free(b.spec, TRUE);
    return ok;
}

static gboolean
parse_hex(const tk *t, ns_color *c)
{
    int n = t->name_len, v[8];
    if (n != 3 && n != 4 && n != 6 && n != 8) return FALSE;
    for (int i = 0; i < n; i++) {
        v[i] = g_ascii_xdigit_value(t->name[i]);
        if (v[i] < 0) return FALSE;
    }
    memset(c, 0, sizeof *c);
    c->space = CS_SRGB;
    c->legacy = TRUE;
    c->alpha = 1.0;
    if (n <= 4) {
        for (int i = 0; i < 3; i++) c->c[i] = v[i] * 17 / 255.0;
        if (n == 4) c->alpha = v[3] * 17 / 255.0;
    } else {
        for (int i = 0; i < 3; i++) c->c[i] = (v[2 * i] * 16 + v[2 * i + 1]) / 255.0;
        if (n == 8) c->alpha = (v[6] * 16 + v[7]) / 255.0;
    }
    return TRUE;
}

static gboolean
parse_color_expr(tks *ts, pctx *cx, cres *out)
{
    memset(out, 0, sizeof *out);
    if (++cx->depth > 24) return FALSE;
    gboolean ok = FALSE;
    tk *t = next_nonws(ts);
    if (t->type == TK_HASH) {
        ts->pos++;
        ok = parse_hex(t, &out->c);
        if (ok) {
            out->resolved = TRUE;
            out->spec = g_string_new(NULL);
            serialize_color(out->spec, &out->c);
        }
    } else if (t->type == TK_IDENT) {
        ts->pos++;
        char name[32];
        int nl = MIN(t->name_len, (int)sizeof name - 1);
        for (int i = 0; i < nl; i++) name[i] = g_ascii_tolower(t->name[i]);
        name[nl] = '\0';
        guint8 r, g, b;
        memset(&out->c, 0, sizeof out->c);
        out->c.space = CS_SRGB;
        out->c.legacy = TRUE;
        out->c.alpha = 1.0;
        if (strcmp(name, "currentcolor") == 0) {
            ok = TRUE;
            if (cx->current) {
                for (int i = 0; i < 3; i++) out->c.c[i] = cx->current[i] / 255.0;
                out->c.alpha = cx->current[3] / 255.0;
                out->resolved = TRUE;
            }
        } else if (strcmp(name, "transparent") == 0) {
            ok = TRUE;
            out->c.alpha = 0.0;
            out->resolved = TRUE;
        } else if (t->name_len < (int)sizeof name && ns_css_named_color(name, &r, &g, &b)) {
            ok = TRUE;
            out->c.c[0] = r / 255.0;
            out->c.c[1] = g / 255.0;
            out->c.c[2] = b / 255.0;
            out->resolved = TRUE;
        }
        if (ok) out->spec = g_string_new(name);
    } else if (t->type == TK_FUNC) {
        ts->pos++;
        static const struct { const char *n; int space; gboolean legacy; const char *canon; } fns[] = {
            { "rgb", CS_SRGB, TRUE, "rgb" }, { "rgba", CS_SRGB, TRUE, "rgb" },
            { "hsl", CS_HSL, TRUE, "hsl" }, { "hsla", CS_HSL, TRUE, "hsl" },
            { "hwb", CS_HWB, FALSE, "hwb" }, { "lab", CS_LAB, FALSE, "lab" },
            { "lch", CS_LCH, FALSE, "lch" }, { "oklab", CS_OKLAB, FALSE, "oklab" },
            { "oklch", CS_OKLCH, FALSE, "oklch" },
        };
        gboolean known = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
            if (tk_is(t, TK_FUNC, fns[i].n)) {
                known = TRUE;
                ok = parse_space_fn(ts, cx, fns[i].space, fns[i].legacy, fns[i].canon, out);
                break;
            }
        if (!known) {
            if (tk_is(t, TK_FUNC, "color")) ok = parse_color_fn(ts, cx, out);
            else if (tk_is(t, TK_FUNC, "color-mix")) ok = parse_color_mix(ts, cx, out);
            else if (tk_is(t, TK_FUNC, "light-dark")) ok = parse_light_dark(ts, cx, out);
            else if (tk_is(t, TK_FUNC, "alpha")) ok = parse_alpha_fn(ts, cx, out);
            else if (tk_is(t, TK_FUNC, "contrast-color")) ok = parse_contrast_color(ts, cx, out);
            else if (tk_is(t, TK_FUNC, "color-layers")) ok = parse_color_layers(ts, cx, out);
        }
    }
    cx->depth--;
    if (!ok && out->spec) {
        g_string_free(out->spec, TRUE);
        out->spec = NULL;
    }
    return ok;
}

static gboolean
parse_top(const char *s, const guint8 *current, cres *out)
{
    if (!s) return FALSE;
    GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(tk), 32);
    gboolean ok = tokenize(s, arr);
    if (ok) {
        tks ts = { &g_array_index(arr, tk, 0), (int)arr->len, 0 };
        pctx cx = { .current = current,
                    .dark = ns_css_get_color_scheme() == NS_CSS_COLOR_SCHEME_DARK };
        ok = parse_color_expr(&ts, &cx, out);
        if (ok && next_nonws(&ts)->type != TK_EOF) {
            g_string_free(out->spec, TRUE);
            out->spec = NULL;
            ok = FALSE;
        }
    }
    g_array_free(arr, TRUE);
    return ok;
}

gboolean
ns_css_color_resolve(const char *s, const guint8 *current, guint8 rgba[4])
{
    cres r;
    if (!parse_top(s, current, &r)) return FALSE;
    gboolean ok = r.resolved;
    if (ok) color_to_rgba8(&r.c, rgba);
    g_string_free(r.spec, TRUE);
    return ok;
}

char *
ns_css_color_specified(const char *s)
{
    cres r;
    if (!parse_top(s, NULL, &r)) return NULL;
    return g_string_free(r.spec, FALSE);
}

char *
ns_css_color_computed(const char *s, const guint8 *current)
{
    cres r;
    if (!parse_top(s, current, &r)) return NULL;
    char *out = NULL;
    if (r.resolved) {
        GString *g = g_string_new(NULL);
        serialize_color(g, &r.c);
        out = g_string_free(g, FALSE);
    }
    g_string_free(r.spec, TRUE);
    return out;
}

char *
ns_css_color_input_value(const char *s, gboolean alpha, gboolean display_p3)
{
    cres r;
    ns_color c = { .space = CS_SRGB, .alpha = 1.0 };
    gboolean parsed = s && parse_top(s, NULL, &r);
    if (parsed) {
        if (r.resolved) c = r.c;
        g_string_free(r.spec, TRUE);
    }
    ns_color t = color_convert(&c, display_p3 ? CS_DISPLAY_P3 : CS_SRGB);
    double a = (c.none & NONE_ALPHA) ? 0.0 : CLAMP(c.alpha, 0.0, 1.0);
    a = floor(a * 255.0 + 0.5) / 255.0;
    GString *out = g_string_new(NULL);
    if (!display_p3) {
        int v[3];
        for (int i = 0; i < 3; i++)
            v[i] = round_channel((t.none & (1u << i)) ? 0.0 : t.c[i]);
        if (!alpha) {
            g_string_append_printf(out, "#%02x%02x%02x", v[0], v[1], v[2]);
            return g_string_free(out, FALSE);
        }
        g_string_append(out, "color(srgb");
        for (int i = 0; i < 3; i++) {
            g_string_append_c(out, ' ');
            fmt_num(out, v[i] / 255.0);
        }
    } else {
        g_string_append(out, "color(display-p3");
        for (int i = 0; i < 3; i++) {
            double x = (t.none & (1u << i)) ? 0.0 : t.c[i];
            g_string_append_c(out, ' ');
            fmt_num(out, CLAMP(x, 0.0, 1.0));
        }
    }
    if (alpha && a < 1.0) {
        g_string_append(out, " / ");
        fmt_num(out, a);
    }
    g_string_append_c(out, ')');
    return g_string_free(out, FALSE);
}

gboolean
ns_css_color_is_legacy_srgb(const char *s)
{
    cres r;
    if (!parse_top(s, NULL, &r)) return FALSE;
    gboolean legacy = r.resolved && r.c.legacy && !r.c.none;
    g_string_free(r.spec, TRUE);
    return legacy;
}
