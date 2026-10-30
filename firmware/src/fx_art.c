/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX page's pictures (ui.c): each model as a plate from an acoustics book drawn in 3D vector style: thin
 * antialiased wire frames in perspective, turning slowly, the parts behind dashed and dim; a measured quantity
 * plotted beside it; a pictogram of the model (or the animal) with its name top left. In the theme's orange, moving
 * with the sound (the wet output's level, rz_view) and the FX knobs. Included by ui.c.
 *
 *   MODAL    a bell turning, its (2,0) mode (the rim going oval), the nodal meridians still; the resonator's partials
 *            as a stem plot on a log-frequency axis (rz_view: the model's own)
 *   SYMP     the played string in front, every resonance behind it as a string of its length (1 / f), swinging
 *   STRING   the normal modes n = 1..4 one behind the other (weighted by the pluck at POSITION), their sum in front
 *   THROAT   the vocal tract as a tube (glottis up, the bend, forward to the lips): its width along it the model's
 *            area function at the VOWEL, its length the animal's; the vocal folds beating, the lips opening; A(x)
 *   FLUTE    the bore as a tube, its rings swelling with the standing wave (one, two, three half waves: the JET
 *            overblowing), the embouchure, the jet bending across its edge; below the pressure along the bore
 *   SYRINX   the trachea down to the syrinx, the two bronchi forking, the labia beating each with its own voice;
 *            beside it the two voices' traces (TWO VOICE: the second apart)
 *   HAWK     the same, the labia rasping (RASP: their traces rough)
 *
 * Integer only (np_sin, Q8 coordinates, Wu's lines). Nothing here touches the audio. */

static uint32_t art_t;                           /* frames drawn (the motion's clock) */
static int32_t art_lv;                           /* the output's level now, 0..256 (fast up, slowly down) */
static uint16_t art_c, art_c2, art_c3;           /* full, secondary, construction (dim) */

static int32_t art_sin(uint32_t ph) { return np_sin(ph); }               /* Q15 */
static int32_t art_cos(uint32_t ph) { return np_sin(ph + 0x40000000u); }
#define Q8(v) ((int32_t)(v) * 256)
#define ART_DEG(d) ((uint32_t)(int32_t)(d) * 11930465u)   /* degrees -> a phase (2^32 / 360) */

static uint32_t art_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 14;
    while (b) {
        if ((r + b) * (r + b) <= v)
            r += b;
        b >>= 1;
    }
    return r;
}
/* a pixel blended over the canvas, a 0..256 */
static void art_px(int32_t x, int32_t y, uint16_t c, int32_t a)
{
    uint16_t *p;
    if (a <= 0 || (uint32_t)x >= cv_w)
        return;
    y += cv_oy;
    if ((uint32_t)y >= cv_h)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    *p = a >= 256 ? swap16(c) : swap16(ux_mix(swap16(*p), c, a * 100 / 256));
}
/* Wu's line, ends in Q8; dash: 0 solid, else on | off << 8 (pixels along the line) */
static void art_ln(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, uint32_t dash)
{
    int32_t dx = x1 - x0, dy = y1 - y0, steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx), g, xi, xe, n = 0;
    uint32_t on = dash & 255u, per = on + (dash >> 8);
    if (steep) { int32_t t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; dx = x1 - x0; dy = y1 - y0; }
    if (x0 > x1) { int32_t t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dx = -dx; dy = -dy; }
    g = dx ? dy * 4096 / dx * 16 : 0;                     /* (the slope, Q16 from Q12: no 64-bit divide) */
    xi = (x0 + 128) >> 8;
    xe = (x1 + 128) >> 8;
    for (; xi <= xe; xi++, n++) {
        int32_t yq = y0 + (int32_t)(((int64_t)(xi * 256 - x0) * g) >> 16), yi = yq >> 8, f = yq & 255;
        if (dash && (uint32_t)n % per >= on)
            continue;
        if (steep) {
            art_px(yi, xi, c, 256 - f);
            art_px(yi + 1, xi, c, f);
        } else {
            art_px(xi, yi, c, 256 - f);
            art_px(xi, yi + 1, c, f);
        }
    }
}
static void art_l(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { art_ln(Q8(x0), Q8(y0), Q8(x1), Q8(y1), c, 0); }

/* a Catmull-Rom curve through n points (Q8), 8 steps a segment; keep: the points drawn collected (for art_hatch) */
#define ART_MAXP 160
static int32_t art_bx[ART_MAXP], art_by[ART_MAXP];
static uint32_t art_bn;
static void art_keep(int32_t x, int32_t y)
{
    if (art_bn < ART_MAXP) {
        art_bx[art_bn] = x;
        art_by[art_bn++] = y;
    }
}
static void art_curve(const int32_t *x, const int32_t *y, uint32_t n, uint16_t c, uint32_t dash, int keep)
{
    uint32_t i, s;
    int32_t px = x[0], py = y[0];
    if (keep)
        art_keep(px, py);
    for (i = 0; i + 1u < n; i++) {
        int32_t x0 = x[i ? i - 1u : 0], y0 = y[i ? i - 1u : 0], x1 = x[i], y1 = y[i], x2 = x[i + 1u], y2 = y[i + 1u];
        int32_t x3 = x[i + 2u < n ? i + 2u : i + 1u], y3 = y[i + 2u < n ? i + 2u : i + 1u];
        for (s = 1; s <= 8u; s++) {
            int32_t t = (int32_t)s * 32, t2 = (t * t) >> 8, t3 = (t2 * t) >> 8;   /* Q8 */
            int32_t qx = (2 * x1 + (((x2 - x0) * t) >> 8) + (((2 * x0 - 5 * x1 + 4 * x2 - x3) * t2) >> 8) +
                          (((3 * x1 - x0 - 3 * x2 + x3) * t3) >> 8)) / 2;
            int32_t qy = (2 * y1 + (((y2 - y0) * t) >> 8) + (((2 * y0 - 5 * y1 + 4 * y2 - y3) * t2) >> 8) +
                          (((3 * y1 - y0 - 3 * y2 + y3) * t3) >> 8)) / 2;
            art_ln(px, py, qx, qy, c, dash);
            if (keep)
                art_keep(qx, qy);
            px = qx;
            py = qy;
        }
    }
}
static void art_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)   /* a circle, Q8 centre, pixels radius */
{
    uint32_t a;
    int32_t px = cx + r * 256, py = cy;
    for (a = 1; a <= 24u; a++) {
        int32_t x = cx + r * art_cos(a * 0x0AAAAAABu) / 128, y = cy + r * art_sin(a * 0x0AAAAAABu) / 128;
        art_ln(px, py, x, y, c, 0);
        px = x;
        py = y;
    }
}
/* log2(hz / 40) Q8 (40 Hz .. 12 kHz: 0 .. 2106): x on a log-frequency axis */
static uint32_t art_oct(uint32_t hz)
{
    uint32_t t = hz * 256u / 40u, l2 = 0;
    while (t >= 512u) { t >>= 1; l2 += 256u; }
    return l2 + t - 256u;
}

/* ---------------------------------------------------------------- 3D view --- */
/* points in Q8 pixels; the view: a turn about the vertical (yaw), then about the horizontal (pitch), perspective */
typedef struct { int32_t x, y, z; } v3_t;
static int32_t a3_cy, a3_sy, a3_cp, a3_sp, a3_ox, a3_oy;
static void a3_view(uint32_t yaw, uint32_t pitch, int32_t ox, int32_t oy)
{
    a3_cy = art_cos(yaw); a3_sy = art_sin(yaw); a3_cp = art_cos(pitch); a3_sp = art_sin(pitch);
    a3_ox = Q8(ox); a3_oy = Q8(oy);
}
static v3_t a3_rot(int32_t x, int32_t y, int32_t z)
{
    v3_t r;
    int32_t x1 = (x * (a3_cy >> 3) + z * (a3_sy >> 3)) >> 12, z1 = (z * (a3_cy >> 3) - x * (a3_sy >> 3)) >> 12;
    r.x = x1;
    r.y = (y * (a3_cp >> 3) - z1 * (a3_sp >> 3)) >> 12;
    r.z = (y * (a3_sp >> 3) + z1 * (a3_cp >> 3)) >> 12;
    return r;
}
static void a3_proj(v3_t p, int32_t *sx, int32_t *sy)   /* (a camera 260 px away) */
{
    int32_t d = Q8(260) + p.z, k = d > Q8(60) ? (int32_t)((uint32_t)Q8(260) * 256u / (uint32_t)d) : 1024;
    *sx = a3_ox + p.x / 16 * k / 16;
    *sy = a3_oy + p.y / 16 * k / 16;
}
/* a segment between rotated points: in front (z under the reference) solid, behind dim and dashed */
static void a3_seg(v3_t a, v3_t b, int32_t zref, uint16_t c)
{
    int32_t x0, y0, x1, y1;
    a3_proj(a, &x0, &y0);
    a3_proj(b, &x1, &y1);
    if ((a.z + b.z) / 2 <= zref)
        art_ln(x0, y0, x1, y1, c, 0);
    else
        art_ln(x0, y0, x1, y1, art_c3, 2u | 2u << 8);
}
/* a tube along a path in the x-y plane (n centre points, Q8; radii Q8): rings every 'every', lengthwise lines */
#define A3_NM 12
static v3_t a3_ring[40][A3_NM];
static int32_t a3_rz[40];
static void a3_tube(const int32_t *px, const int32_t *py, const int32_t *pr, uint32_t n, uint32_t every, uint16_t c,
                    uint16_t cl)
{
    uint32_t i, k;
    if (n > 40u)
        n = 40u;
    for (i = 0; i < n; i++) {
        uint32_t a = i ? i - 1u : 0u, b = i + 1u < n ? i + 1u : i;
        int32_t tx = (px[b] - px[a]) / 16, ty = (py[b] - py[a]) / 16, l = (int32_t)art_isqrt((uint32_t)(tx * tx + ty * ty));
        int32_t nx = l ? -ty * 256 / l : 256, ny = l ? tx * 256 / l : 0;   /* the normal in the plane, Q8 */
        a3_rz[i] = a3_rot(px[i], py[i], 0).z;
        for (k = 0; k < A3_NM; k++) {
            uint32_t th = k * (0xFFFFFFFFu / A3_NM);
            int32_t ct = art_cos(th) >> 7, st = art_sin(th) >> 7;   /* Q8 */
            int32_t r = pr[i];
            a3_ring[i][k] = a3_rot(px[i] + (r * ct >> 8) * nx / 256, py[i] + (r * ct >> 8) * ny / 256, r * st >> 8);
        }
    }
    for (i = 0; i < n; i++)
        if (i % every == 0u || i + 1u == n)
            for (k = 0; k < A3_NM; k++)
                a3_seg(a3_ring[i][k], a3_ring[i][(k + 1u) % A3_NM], a3_rz[i], c);
    for (k = 0; k < A3_NM; k += 2u)
        for (i = 0; i + 1u < n; i++)
            a3_seg(a3_ring[i][k], a3_ring[i + 1u][k], (a3_rz[i] + a3_rz[i + 1u]) / 2, cl);
}

/* ------------------------------------------------------------- pictograms --- */
/* each a 24 px line drawing, half pixels: 1 curve n (x y)..., 2 lines n (x y)..., 3 circle x y r, 4 dot x y, 0 end */
#include "fx_icons.h"
static void art_icon(const int8_t *p, int32_t ox, int32_t oy, uint16_t c)
{
    int32_t x[24], y[24];
    while (*p) {
        int32_t op = *p++, n, i;
        if (op == 1 || op == 2) {
            n = *p++;
            for (i = 0; i < n; i++, p += 2)
                if (i < 24) { x[i] = Q8(ox) + p[0] * 128; y[i] = Q8(oy) + p[1] * 128; }
            n = n > 24 ? 24 : n;
            if (op == 1)
                art_curve(x, y, (uint32_t)n, c, 0, 0);
            else
                for (i = 1; i < n; i++)
                    art_ln(x[i - 1], y[i - 1], x[i], y[i], c, 0);
        } else if (op == 3) {
            art_ring(Q8(ox) + p[0] * 128, Q8(oy) + p[1] * 128, p[2] / 2 > 0 ? p[2] / 2 : 1, c);
            p += 3;
        } else {
            int32_t dx = ox + p[0] / 2, dy = oy + p[1] / 2;
            art_px(dx, dy, c, 256); art_px(dx + 1, dy, c, 160); art_px(dx, dy + 1, c, 160); art_px(dx + 1, dy + 1, c, 100);
            p += 2;
        }
    }
}
static void art_title(const int8_t *icon, const char *name)   /* the pictogram top left, its name beside it */
{
    art_icon(icon, 3, 3, art_c);
    cv_text(32, 6, &AF_S, name, art_c2);
}

/* ------------------------------------------------------------------ MODAL --- */
static const int8_t BELL_O[9][2] = {{0, 6}, {12, 7}, {19, 12}, {21, 24}, {22, 40}, {25, 54}, {31, 66}, {38, 74}, {41, 78}};
static void art_bell(void)
{
    const uint32_t NM = 16u, NP = 48u;
    int32_t amp = art_lv * 15 * art_sin(art_t * 0x2E000000u) / 32768;   /* the (2,0) mode, Q12 of the radius */
    uint32_t i, j, k;
    a3_view(art_t * 0x00A00000u, (uint32_t)-ART_DEG(18), 62, 75);
    for (i = 0; i < NM; i++) {                                   /* the meridians */
        uint32_t th = i * (0xFFFFFFFFu / NM);
        v3_t prev = {0, 0, 0};
        for (j = 0; j < 9u; j++) {
            int32_t d = 4096 + amp * art_cos(2u * th) / 32768 * BELL_O[j][1] / 78, r = BELL_O[j][0] * 300 * d / 4096;
            v3_t p = a3_rot(r * (art_cos(th) >> 7) >> 8, (BELL_O[j][1] - 44) * 300, r * (art_sin(th) >> 7) >> 8);
            if (j)
                a3_seg(prev, p, 0, art_c2);
            prev = p;
        }
    }
    for (j = 1; j < 9u; j++) {                                   /* the parallels, the lip bright */
        v3_t prev = {0, 0, 0};
        for (k = 0; k <= NP; k++) {
            uint32_t th = k * (0xFFFFFFFFu / NP);
            int32_t d = 4096 + amp * art_cos(2u * th) / 32768 * BELL_O[j][1] / 78, r = BELL_O[j][0] * 300 * d / 4096;
            v3_t p = a3_rot(r * (art_cos(th) >> 7) >> 8, (BELL_O[j][1] - 44) * 300, r * (art_sin(th) >> 7) >> 8);
            if (k)
                a3_seg(prev, p, 0, j == 8u ? art_c : art_c2);
            prev = p;
        }
    }
    for (i = 0; i < 4u; i++) {                                   /* the (2,0) mode's nodal meridians, still */
        uint32_t th = 0x20000000u + i * 0x40000000u;
        v3_t prev = {0, 0, 0};
        int32_t x0 = 0, y0 = 0, x1, y1, zs = 0;
        for (j = 0; j < 9u; j++)
            zs += a3_rot(BELL_O[j][0] * (art_cos(th) >> 7) * 300 >> 8, 0, BELL_O[j][0] * (art_sin(th) >> 7) * 300 >> 8).z;
        if (zs > 0)
            continue;
        for (j = 0; j < 9u; j++) {
            v3_t p = a3_rot(BELL_O[j][0] * (art_cos(th) >> 7) * 300 >> 8, (BELL_O[j][1] - 44) * 300,
                            BELL_O[j][0] * (art_sin(th) >> 7) * 300 >> 8);
            a3_proj(p, &x1, &y1);
            if (j)
                art_ln(x0, y0, x1, y1, art_c, 3u | 2u << 8);
            x0 = x1; y0 = y1; prev = p;
        }
        (void)prev;
    }
    {   /* the axis */
        int32_t x0, y0, x1, y1;
        a3_proj(a3_rot(0, Q8(-56), 0), &x0, &y0);
        a3_proj(a3_rot(0, Q8(54), 0), &x1, &y1);
        art_ln(x0, y0, x1, y1, art_c3, 6u | 3u << 8);
    }
    {   /* the partials: a stem plot on a log-frequency axis (40 Hz .. 12 kHz), the model's own */
        const int32_t x0 = 128, x1 = 234, yb = PLOT_H - 16, ht = 80;
        uint32_t nn = rz_view.n > 16u ? 16u : rz_view.n;
        static const uint16_t HZ[3] = {100, 1000, 10000};
        static const char *const HL[3] = {"100", "1k", "10k"};
        art_l(x0, yb, x1, yb, art_c3);
        art_l(x0, yb, x0, yb - ht, art_c3);
        for (k = 0; k < 3u; k++) {
            int32_t x = x0 + (int32_t)(art_oct(HZ[k]) * (uint32_t)(x1 - x0) / 2106u);
            art_l(x, yb, x, yb + 3, art_c3);
            cv_text(k == 2u ? x - 18 : x - 4, yb + 2, &AF_S, HL[k], art_c3);
        }
        cv_text(x0 + 4, yb - ht - 4, &AF_S, "A", art_c3);
        for (k = 0; k < nn && sys.fx_mix; k++) {
            uint32_t f = rz_view.f[k], hz;
            int32_t x, h;
            if (!f)
                continue;
            hz = (uint32_t)(((uint64_t)f * 44100u) >> 32);
            if (hz < 40u || hz > 12000u)
                continue;
            x = x0 + (int32_t)(art_oct(hz) * (uint32_t)(x1 - x0) / 2106u);
            h = 3 + (rz_view.a[k] < 0 ? ht - 3 : rz_view.a[k] * (ht - 3) / 32767);
            art_l(x, yb, x, yb - h, art_c);
            art_ring(Q8(x), Q8(yb - h), 2, art_c);
        }
    }
    art_title(ICON_BELL, "BELL");
}

/* ------------------------------------------------------- STRING and SYMP --- */
/* a string in 3D from x0 to x1 (Q8, centred on 0) at height y and depth z: d(u) sideways, its nodes */
static void a3_string(int32_t x0, int32_t x1, int32_t y, int32_t z, const int32_t *amp, uint32_t nm, uint32_t speed,
                      int32_t scale, uint16_t c, int nodes)
{
    v3_t prev = {0, 0, 0};
    uint32_t i, k;
    for (i = 0; i <= 48u; i++) {
        uint32_t u = i * (0x80000000u / 48u);
        int32_t d = 0, e = 0;
        v3_t p;
        for (k = 0; k < nm; k++) {
            int32_t s = art_sin((k + 1u) * u), w = (k + 1u) * art_t * speed;
            d += (int32_t)(((int64_t)amp[k] * art_cos((uint32_t)w) >> 15) * s >> 15);
            e += (int32_t)(((int64_t)amp[k] * art_sin((uint32_t)w) >> 15) * s >> 17);   /* (a little the other way) */
        }
        p = a3_rot(x0 + (x1 - x0) / 48 * (int32_t)i, y - d / 128 * scale / 256, z + e / 128 * scale / 256);
        if (i)
            a3_seg(prev, p, Q8(400), c);
        prev = p;
    }
    if (nodes && nm == 1u) {
        int32_t sx, sy;
        a3_proj(a3_rot(x0, y, z), &sx, &sy);
        art_ring(sx, sy, 1, art_c2);
        a3_proj(a3_rot(x1, y, z), &sx, &sy);
        art_ring(sx, sy, 1, art_c2);
    }
}
static void art_string_model(void)
{
    uint32_t k, pp = (sys.fx_pos & 127u) * (0x80000000u / 128u) + 0x01000000u;
    int32_t amp[4], one[4] = {0, 0, 0, 0}, mx = 1, x0 = Q8(-96), x1 = Q8(96), sx, sy, sx2, sy2;
    a3_view((uint32_t)ART_DEG(-22) + (uint32_t)(art_sin(art_t * 0x00500000u) * 3000), (uint32_t)-ART_DEG(12), 122, 40);
    for (k = 0; k < 4u; k++) {                                   /* each mode's weight: sin(n pi p) / n^2 */
        amp[k] = art_sin((k + 1u) * pp) / (int32_t)((k + 1u) * (k + 1u));
        mx = amp[k] > mx ? amp[k] : -amp[k] > mx ? -amp[k] : mx;
    }
    for (k = 0; k < 4u; k++)
        amp[k] = amp[k] * 32767 / mx;
    for (k = 0; k < 4u; k++) {                                   /* the modes, one behind the other */
        int32_t z = Q8(14) + (int32_t)k * Q8(10), yk = Q8(24) + (int32_t)k * Q8(18), lv = art_lv * amp[k] / 256, j;
        one[k] = lv;
        {   /* (this mode alone, at its own speed) */
            int32_t a1[4] = {0, 0, 0, 0};
            a1[k] = lv;
            a3_string(x0, x1, yk, z, a1, k + 1u, 0x26000000u, 9 * 256, art_c2, 0);
        }
        for (j = 0; j <= (int32_t)k + 1; j++) {                  /* its nodes */
            a3_proj(a3_rot(x0 + (x1 - x0) / ((int32_t)k + 1) * j, yk, z), &sx, &sy);
            art_ring(sx, sy, 1, art_c2);
        }
        a3_proj(a3_rot(x0, yk, z), &sx, &sy);
        a3_proj(a3_rot(x1, yk, z), &sx2, &sy2);
        art_ln(sx, sy + Q8(4), sx2, sy2 + Q8(4), art_c3, 1u | 3u << 8);
    }
    (void)one;
    for (k = 0; k < 4u; k++)
        amp[k] = amp[k] * art_lv / 256;
    a3_string(x0, x1, Q8(4), 0, amp, 4, 0x26000000u, 14 * 256, art_c, 0);   /* the string: their sum, in front */
    for (k = 0; k < 2u; k++) {                                   /* the bridges */
        int32_t bx = k ? x1 : x0, ax, ay, bx2, by2, cx2, cy2;
        a3_proj(a3_rot(bx - Q8(5), Q8(12), 0), &ax, &ay);
        a3_proj(a3_rot(bx, Q8(4), 0), &bx2, &by2);
        a3_proj(a3_rot(bx + Q8(5), Q8(12), 0), &cx2, &cy2);
        art_ln(ax, ay, bx2, by2, art_c, 0); art_ln(bx2, by2, cx2, cy2, art_c, 0); art_ln(ax, ay, cx2, cy2, art_c, 0);
    }
    {   /* where it is plucked */
        int32_t xp = x0 + (x1 - x0) / 127 * (int32_t)(sys.fx_pos & 127u);
        a3_proj(a3_rot(xp, Q8(4), 0), &sx, &sy);
        a3_proj(a3_rot(xp, Q8(80), Q8(44)), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c3, 1u | 3u << 8);
    }
    art_title(ICON_STRING, "STRING");
}
static void art_sitar(void)
{
    uint32_t k, nn = rz_view.n > 12u ? 12u : rz_view.n, fmin = 0xFFFFFFFFu, row = 0;
    int32_t x0 = Q8(-100), x1 = Q8(100), a[1], sx, sy, sx2, sy2;
    a3_view((uint32_t)ART_DEG(-18) + (uint32_t)(art_sin(art_t * 0x00500000u) * 3000), (uint32_t)-ART_DEG(10), 122, 46);
    a[0] = art_lv * 128;
    a3_string(x0, x1, 0, 0, a, 1, 0x26000000u, 8 * 256, art_c, 1);   /* the played string, in front */
    for (k = 0; k < nn; k++)
        if (rz_view.f[k] && rz_view.f[k] < fmin)
            fmin = rz_view.f[k];
    for (k = 0; k < nn && sys.fx_mix; k++) {                     /* every resonance a string of its length, behind */
        uint32_t f = rz_view.f[k];
        int32_t len, sym = rz_view.a[k] < 0;
        if (!f)
            continue;
        len = (int32_t)((fmin >> 12) * 200u / ((f >> 12) | 1u));
        len = len < 16 ? 16 : len;
        a[0] = sym ? art_lv * art_lv / 2 : rz_view.a[k] * art_lv / 64;
        if (row >= 8u)
            break;
        a3_string(x1 - Q8(len), x1, Q8(12) + (int32_t)row * Q8(8), Q8(10) + (int32_t)row * Q8(6), a, 1,
                  0x26000000u + k * 0x05000000u,
                  5 * 256, sym ? art_c : art_c2, 1);
        row++;
    }
    {   /* the bridge (the jawari) under the played string; the nut holding them all */
        int32_t r = row ? (int32_t)row - 1 : 0;
        a3_proj(a3_rot(x0, Q8(6), Q8(-6)), &sx, &sy);
        a3_proj(a3_rot(x0, Q8(6), Q8(6)), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c, 0);
        a3_proj(a3_rot(x1, Q8(-2), Q8(-4)), &sx, &sy);
        a3_proj(a3_rot(x1, Q8(14) + r * Q8(8), Q8(12) + r * Q8(6)), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c2, 0);
    }
    art_title(ICON_SITAR, "SITAR");
}

/* ----------------------------------------------------------------- THROAT --- */
static const int8_t *const ANI_ICON[WA_COUNT] = {ICON_CAT, ICON_MONKEY, ICON_HUMAN, ICON_DOG, ICON_WOLF, ICON_COW,
                                                 ICON_LION, ICON_ELEPHANT};
static void art_throat(void)
{
    uint32_t an = (sys.fx_struct & 127u) * WA_COUNT / 128u, i;
    uint32_t vq = (sys.fx_pos & 127u) * (WG_NV - 1u) * 256u / 127u, v0 = vq >> 8, v1 = v0 + 1u < WG_NV ? v0 + 1u : v0;
    int32_t vf = (int32_t)(vq & 255u), nk = (int32_t)WG_ANI[an].n * (int32_t)WG_ANI[an].k;
    int32_t lv_ = 30 + nk / 3, R = 14, lh = 34 + nk / 2, len, px[33], py[33], pr[33];
    const int32_t N = 32;
    lv_ = lv_ > 58 ? 58 : lv_;
    lh = lh > 92 ? 92 : lh;
    len = lv_ + R * 157 / 100 + lh;
    a3_view((uint32_t)ART_DEG(-26) + (uint32_t)(art_sin(art_t * 0x00700000u) * 6000), (uint32_t)-ART_DEG(14), 92, 70);
    for (i = 0; i <= (uint32_t)N; i++) {                         /* the tube: glottis up, the bend, forward */
        int32_t s = len * (int32_t)i / N, ar;
        uint32_t q = i * 15u * 256u / (uint32_t)N, j = q >> 8, j1 = j + 1u < 16u ? j + 1u : j;
        int32_t f = (int32_t)(q & 255u);
        int32_t a0 = WG_SHAPE[v0][j] + ((WG_SHAPE[v0][j1] - WG_SHAPE[v0][j]) * f >> 8);
        int32_t a1 = WG_SHAPE[v1][j] + ((WG_SHAPE[v1][j1] - WG_SHAPE[v1][j]) * f >> 8);
        ar = a0 + ((a1 - a0) * vf >> 8);
        if (i == (uint32_t)N)                                    /* (the lips opening with the voice) */
            ar += art_lv * (int32_t)WG_ANI[an].wah / 512 * (art_sin(art_t * 0x18000000u) + 32768) / 65536;
        pr[i] = (int32_t)art_isqrt((uint32_t)ar * 236000u);
        if (s < lv_) {
            px[i] = Q8(-40); py[i] = Q8(44 - s);
        } else if (s < lv_ + R * 157 / 100) {
            uint32_t a = (uint32_t)(s - lv_) * (0x40000000u / (uint32_t)(R * 157 / 100));
            px[i] = Q8(-40 + R) - R * art_cos(a) / 128;
            py[i] = Q8(44 - lv_) - R * art_sin(a) / 128;
        } else {
            px[i] = Q8(-40 + R + (s - lv_ - R * 157 / 100));
            py[i] = Q8(44 - lv_ - R);
        }
    }
    a3_tube(px, py, pr, (uint32_t)N + 1u, 2, art_c, art_c2);
    {   /* the vocal folds in the first ring: two lips, the gap beating */
        int32_t g = art_lv * (art_sin(art_t * 0x4C000000u) + 32768) / 65536 * 3, k, sx, sy, sx2, sy2;
        for (k = -1; k <= 1; k += 2) {
            a3_proj(a3_rot(px[1] + k * pr[1], py[1], 0), &sx, &sy);
            a3_proj(a3_rot(px[1] + k * g, py[1] - Q8(1), 0), &sx2, &sy2);
            art_ln(sx, sy, sx2, sy2, art_c, 0);
            art_ln(sx, sy + 256, sx2, sy2 + 256, art_c, 0);
        }
    }
    {   /* beside it: the area function A(x), glottis (G) to lips (L) */
        const int32_t x0 = 184, x1 = 234, yc = 98;
        int32_t k, prev = 0;
        art_ln(Q8(x0 - 3), Q8(yc), Q8(x1 + 3), Q8(yc), art_c3, 6u | 3u << 8);
        for (k = 0; k < 16; k++) {
            int32_t a0 = WG_SHAPE[v0][k] + (((int32_t)WG_SHAPE[v1][k] - WG_SHAPE[v0][k]) * vf >> 8);
            int32_t h = 1 + (int32_t)art_isqrt((uint32_t)a0 * 4u) * 7 / 10, xa = x0 + (x1 - x0) * k / 16;
            int32_t xb = x0 + (x1 - x0) * (k + 1) / 16;
            if (k) {
                art_l(xa, yc - prev, xa, yc - h, art_c);
                art_l(xa, yc + prev, xa, yc + h, art_c);
            }
            art_l(xa, yc - h, xb, yc - h, art_c);
            art_l(xa, yc + h, xb, yc + h, art_c);
            prev = h;
        }
        cv_text(x0, yc - 34, &AF_S, "A(x)", art_c3);
    }
    art_title(ANI_ICON[an], WG_ANIMAL[an]);
}

/* ------------------------------------------------------------------ FLUTE --- */
static void art_flute(void)
{
    uint32_t nh = 1u + (sys.fx_struct & 127u) * 3u / 128u, i;
    int32_t px[25], py[25], pr[25], cw = art_cos(art_t * 0x30000000u), sx, sy, sx2, sy2;
    const int32_t x0 = Q8(-104), x1 = Q8(104), hx = Q8(-70), gy = 100;
    a3_view((uint32_t)ART_DEG(-24) + (uint32_t)(art_sin(art_t * 0x00500000u) * 3000), (uint32_t)-ART_DEG(24), 126, 66);
    for (i = 0; i <= 24u; i++) {                                 /* the bore, its rings swelling with the pressure */
        uint32_t u = (uint32_t)(i * nh) * (0x80000000u / 24u);
        int32_t p = art_lv * cw / 32768 * art_sin(u) / 32768;   /* -256 .. 256 */
        px[i] = x0 + (x1 - x0) / 24 * (int32_t)i;
        py[i] = 0;
        pr[i] = Q8(9) + p * 3;
    }
    a3_tube(px, py, pr, 25, 1, art_c2, art_c3);
    {   /* the embouchure hole on top, the lips' jet bending across its edge */
        int32_t k, ox = 0, oy = 0;
        for (k = 0; k <= 12; k++) {
            uint32_t th = (uint32_t)k * (0xFFFFFFFFu / 12u);
            a3_proj(a3_rot(hx + (art_cos(th) >> 7) * 4, Q8(-9), (art_sin(th) >> 7) * 3), &sx, &sy);
            if (k)
                art_ln(ox, oy, sx, sy, art_c, 0);
            ox = sx; oy = sy;
        }
        for (k = 0; k <= 16; k++) {
            int32_t t = k, wv = art_lv * t * art_sin(art_t * 0x50000000u - (uint32_t)k * 0x16000000u) / (32768 * 16) * 3;
            a3_proj(a3_rot(hx - Q8(22) + Q8(22) * t / 16, Q8(-24) + Q8(15) * t / 16 + wv, 0), &sx, &sy);
            if (k)
                art_ln(ox, oy, sx, sy, art_c, 0);
            ox = sx; oy = sy;
        }
        a3_proj(a3_rot(hx - Q8(26), Q8(-30), 0), &sx, &sy);       /* the lips */
        a3_proj(a3_rot(hx - Q8(22), Q8(-24), 0), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c, 0);
        a3_proj(a3_rot(hx - Q8(26), Q8(-18), 0), &sx, &sy);
        art_ln(sx, sy, sx2, sy2, art_c, 0);
    }
    {   /* below: the pressure along the bore, its envelope, its nodes */
        const int32_t gx0 = 60, gx1 = 234;
        int32_t ppx = Q8(gx0), pv = Q8(gy), pe = Q8(gy), pf = Q8(gy);
        art_l(gx0, gy, gx1, gy, art_c3);
        cv_text(gx0 - 12, gy - 20, &AF_S, "P", art_c3);
        for (i = 0; i <= 48u; i++) {
            int32_t xx = Q8(gx0) + (Q8(gx1) - Q8(gx0)) / 48 * (int32_t)i, s = art_sin(i * nh * (0x80000000u / 48u));
            int32_t yv = Q8(gy) - 12 * art_lv * cw / 32768 * s / 32768, ye = Q8(gy) - 12 * 256 * s / 32768;
            int32_t yf = Q8(gy) + 12 * 256 * s / 32768;
            if (i) {
                art_ln(ppx, pv, xx, yv, art_c, 0);
                art_ln(ppx, pe, xx, ye, art_c3, 2u | 2u << 8);
                art_ln(ppx, pf, xx, yf, art_c3, 2u | 2u << 8);
            }
            ppx = xx; pv = yv; pe = ye; pf = yf;
        }
        for (i = 0; i <= nh; i++)
            art_ring(Q8(gx0) + (Q8(gx1) - Q8(gx0)) / (int32_t)nh * (int32_t)i, Q8(gy), 1, art_c2);
    }
    if (art_lv > 8) {                                            /* the breath: dust at the jet */
        uint32_t n = (uint32_t)art_lv * (sys.fx_pos & 127u) / 1024u, r = art_t * 2654435761u;
        for (i = 0; i < n; i++) {
            r = r * 1664525u + 1013904223u;
            a3_proj(a3_rot(hx - Q8(20) + (int32_t)(r >> 24) * 72, Q8(-22) + (int32_t)((r >> 16) & 255u) * 40, 0), &sx, &sy);
            art_px(sx >> 8, sy >> 8, art_c2, 180);
        }
    }
    art_title(ICON_FLUTE, "FLUTE");
}

/* -------------------------------------------------------- SYRINX and HAWK --- */
static void art_syrinx(int hawk)
{
    int32_t beat = (int32_t)(sys.fx_pos & 127u), px[16], py[16], pr[16], i, sx, sy, sx2, sy2;
    uint32_t ph1 = art_t * 0x3C000000u, ph2 = art_t * (0x3C000000u + (uint32_t)beat * 0x00300000u), rr = art_t * 2246822519u;
    int32_t g1 = art_lv * (art_sin(ph1) + 32768) / 65536 * 3, g2 = art_lv * (art_sin(ph2) + 32768) / 65536 * 3;
    if (hawk && art_lv > 16) {                                   /* (the rasp: the labia shaken) */
        g1 += (int32_t)((rr >> 28) & 7u) * beat * 2;
        g2 += (int32_t)((rr >> 24) & 7u) * beat * 2;
    }
    a3_view((uint32_t)ART_DEG(-30) + (uint32_t)(art_sin(art_t * 0x00600000u) * 5000), (uint32_t)-ART_DEG(16), 70, 70);
    for (i = 0; i < 12; i++) {                                   /* the trachea: down to the syrinx, widening */
        px[i] = 0;
        py[i] = Q8(-44) + Q8(4) * i;
        pr[i] = Q8(7) + (i > 8 ? Q8(i - 8) * 3 / 2 : 0);
    }
    a3_tube(px, py, pr, 12, 1, art_c, art_c2);
    for (i = 0; i < 2; i++) {                                    /* the bronchi, forking */
        int32_t s = i ? 1 : -1, k;
        for (k = 0; k < 9; k++) {
            px[k] = s * (Q8(8) + Q8(k) * 3 / 2 + Q8(k * k) / 10);
            py[k] = Q8(4) + Q8(6) * k;
            pr[k] = Q8(5);
        }
        a3_tube(px, py, pr, 9, 1, art_c, art_c2);
    }
    for (i = 0; i < 2; i++) {                                    /* the labia at the fork, each side beating */
        int32_t s = i ? 1 : -1, g = i ? g2 : g1;
        a3_proj(a3_rot(s * Q8(13), Q8(0), 0), &sx, &sy);
        a3_proj(a3_rot(s * Q8(8) - s * g / 2, Q8(5), 0), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c, 0);
        a3_proj(a3_rot(s * Q8(10), Q8(10), 0), &sx, &sy);
        art_ln(sx2, sy2, sx, sy, art_c, 0);
        a3_proj(a3_rot(s * Q8(2), Q8(2), 0), &sx, &sy);
        a3_proj(a3_rot(s * Q8(5) + s * g / 2, Q8(7), 0), &sx2, &sy2);
        art_ln(sx, sy, sx2, sy2, art_c, 0);
    }
    if (art_lv > 12) {                                           /* the air: up the bronchi, through the trachea */
        uint32_t nd = (uint32_t)art_lv / 20u, k;
        for (k = 0; k < nd; k++) {
            uint32_t ph = (art_t * 4u + k * 29u) % 82u;
            int32_t sd = (k & 1u) ? 1 : -1, ax, ay;
            if (ph < 40u) { ay = Q8(52) - Q8((int32_t)ph * 52 / 40); ax = sd * (Q8(8) + Q8(40 - (int32_t)ph) / 3); }
            else { ay = Q8(-2) - Q8((int32_t)ph - 40); ax = sd * Q8(2); }
            a3_proj(a3_rot(ax, ay, 0), &sx, &sy);
            art_px(sx >> 8, sy >> 8, art_c, 220);
        }
    }
    {   /* beside it: the two voices' traces */
        const int32_t x0 = 138, x1 = 234;
        uint32_t k;
        for (k = 0; k < 2u; k++) {
            int32_t yc = 48 + (int32_t)k * 44, j, ppx = Q8(x0), ppy = Q8(yc);
            uint32_t fr = 0x10000000u + (k ? (uint32_t)beat * 0x00080000u : 0u);
            art_l(x0, yc, x1, yc, art_c3);
            cv_text(x0 - 12, yc - 8, &AF_S, k ? "R" : "L", art_c3);
            for (j = 1; j <= 60; j++) {
                int32_t xx = Q8(x0) + (Q8(x1) - Q8(x0)) / 60 * j;
                int32_t v = art_sin((uint32_t)j * fr + art_t * 0x09000000u * (k + 1u)) / 128 * art_lv / 256 * 16;
                if (hawk && art_lv > 16)
                    v += ((int32_t)((((uint32_t)j * 2654435761u + art_t * 40503u) >> 24) & 255u) - 128) * beat / 16;
                art_ln(ppx, ppy, xx, Q8(yc) - v, art_c, 0);
                ppx = xx;
                ppy = Q8(yc) - v;
            }
        }
    }
    art_title(hawk ? ICON_HAWK : ICON_SONGBIRD, hawk ? "HAWK" : "SONGBIRD");
}

/* the FX page's picture */
static void art_draw(void)
{
    uint32_t i, w = rz_view.w;
    static int32_t scale = 2000;
    int32_t pk = 0;
    for (i = 0; i < 128u; i++) {
        int32_t v = rz_view.wave[(w - 128u + i) & (RZ_WAVE - 1u)];
        pk = v > pk ? v : -v > pk ? -v : pk;
    }
    scale = pk > scale ? pk : scale - (scale - (pk > 2000 ? pk : 2000)) / 16;
    pk = sys.fx_mix ? pk * 256 / (scale > 1 ? scale : 1) : 0;
    art_lv = pk > art_lv ? pk : art_lv - (art_lv - pk) / 6;
    art_t++;
    art_c = sys.fx_mix ? T_THEME : ux_mix(T_BG, T_THEME, 45);
    art_c2 = ux_mix(T_BG, art_c, 75);
    art_c3 = ux_mix(T_BG, art_c, 45);
    cv_begin(240, PLOT_H, T_BG);
    switch (sys.fx_model % RZ_COUNT) {
    case RZ_MODAL: art_bell(); break;
    case RZ_SYMP: art_sitar(); break;
    case RZ_STRING: art_string_model(); break;
    case RZ_THROAT: art_throat(); break;
    case RZ_FLUTE: art_flute(); break;
    case RZ_SYRINX: art_syrinx(0); break;
    default: art_syrinx(1); break;
    }
    if (!sys.fx_mix)
        cv_text(160, 4, &AF_S, "MIX OFF", T_MID);
    cv_blit(0, PLOT_Y);
}
