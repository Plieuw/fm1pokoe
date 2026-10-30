/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX page's pictures (ui.c): each model as a plate from an acoustics or anatomy book: thin antialiased lines,
 * cut surfaces hatched, displaced shapes dashed, a measured quantity plotted beside it; in the theme's orange,
 * moving with the sound (the wet output's level, rz_view) and the FX knobs. Included by ui.c.
 *
 *   MODAL    a bell in half section (the wall hatched, the centre line dash-dot), its (2,0) mode dashed; beside it
 *            the resonator's partials as a stem plot on a log-frequency axis (rz_view: the model's own)
 *   SYMP     the played string, and under it every resonance of the instrument as a string of its length (1 / f),
 *            each swinging with its level: the sympathetic ones answering
 *   STRING   the normal modes n = 1..4 of the string plucked at POSITION: each its shape, its envelope dashed, its
 *            nodes; on top their sum, the string itself
 *   THROAT   a midsagittal section of the head: nasal cavity, palate and velum, the tongue in the VOWEL's position,
 *            teeth, lips, the jaw opening with the level, the larynx with the vocal folds beating, the cervical
 *            vertebrae; the muzzle per animal (a cow's dental pad, the carnivores' canines, the elephant's trunk).
 *            Beside it the area function A(x) of the model's tube, glottis to lips, as a tube in section
 *   FLUTE    the head joint in section: cork, embouchure, the jet from the lips bending across the edge; under it
 *            the pressure's standing wave along the bore (one, two, three half waves: the JET overblowing)
 *   SYRINX   a songbird's syrinx, ventral view: the trachea's rings, the two bronchi, the labia on each side beating
 *            with their own voice; beside it the two voices' traces (TWO VOICE: the second apart)
 *   HAWK     the same, the labia rasping (RASP: their traces rough)
 *
 * Integer only. Lines: Wu's antialiased, Q8 coordinates. Nothing here touches the audio. */

static uint32_t art_t;                           /* frames drawn (the motion's clock) */
static int32_t art_lv;                           /* the output's level now, 0..256 (fast up, slowly down) */
static uint16_t art_c, art_c2, art_c3;           /* full, secondary, construction (dim) */

static int32_t art_sin(uint32_t ph) { return np_sin(ph); }               /* Q15 */
static int32_t art_cos(uint32_t ph) { return np_sin(ph + 0x40000000u); }
#define Q8(v) ((int32_t)(v) * 256)

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
/* the points kept (a closed outline) hatched at 45 degrees: a cut surface */
static void art_hatch(uint16_t c, uint32_t step)
{
    int32_t y, y0 = 9999, y1 = -9999;
    uint32_t i;
    if (art_bn < 3u)
        return;
    for (i = 0; i < art_bn; i++) {
        y0 = art_by[i] >> 8 < y0 ? art_by[i] >> 8 : y0;
        y1 = art_by[i] >> 8 > y1 ? art_by[i] >> 8 : y1;
    }
    for (y = y0; y <= y1; y++) {
        int32_t xs[16], ns = 0, yq = y * 256 + 128, a, b, x;
        for (i = 0; i < art_bn && ns < 16; i++) {
            uint32_t j = i + 1u < art_bn ? i + 1u : 0u;
            int32_t ya = art_by[i], yb = art_by[j];
            if ((ya <= yq && yb > yq) || (yb <= yq && ya > yq)) {
                int32_t d = (yb - ya) / 16;
                xs[ns++] = art_bx[i] + (d ? (art_bx[j] - art_bx[i]) * ((yq - ya) / 16) / d : 0);
            }
        }
        for (a = 1; a < ns; a++)                          /* (a few: insertion) */
            for (b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int32_t t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (a = 0; a + 1 < ns; a += 2)
            for (x = (xs[a] + 255) >> 8; x <= xs[a + 1] >> 8; x++)
                if ((uint32_t)(x + y) % step == 0u)
                    art_px(x, y, c, 256);
    }
}
/* a closed polygon of n points (Q8): outlined, hatched */
static void art_section(const int32_t *x, const int32_t *y, uint32_t n, uint16_t c, uint32_t step)
{
    uint32_t i;
    art_bn = 0;
    for (i = 0; i < n; i++) {
        art_ln(x[i], y[i], x[(i + 1u) % n], y[(i + 1u) % n], c, 0);
        art_keep(x[i], y[i]);
    }
    art_hatch(art_c3, step);
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

/* ------------------------------------------------------------------ MODAL --- */
/* the bell's profile (one side, crown to lip): outer and inner wall, pixels from the axis / the crown */
static const int8_t BELL_O[9][2] = {{0, 6}, {12, 7}, {19, 12}, {21, 24}, {22, 40}, {25, 54}, {31, 66}, {38, 74}, {41, 78}};
static const int8_t BELL_I[9][2] = {{0, 10}, {10, 11}, {16, 15}, {18, 26}, {19, 41}, {22, 54}, {27, 65}, {33, 72}, {37, 76}};
static void art_bell(void)
{
    const int32_t cx = 52, cy = 10, n = 9;
    int32_t xo[9], yo[9], xi[9], yi[9], i, d = art_lv * art_sin(art_t * 0x2E000000u) / 32768;
    art_ln(Q8(cx), Q8(2), Q8(cx), Q8(PLOT_H - 4), art_c3, 6u | 3u << 8);   /* the axis, dash-dot */
    /* the right half in section: outer, lip, inner; hatched */
    art_bn = 0;
    for (i = 0; i < n; i++) { xo[i] = Q8(cx) + BELL_O[i][0] * 300; yo[i] = Q8(cy) + BELL_O[i][1] * 320; }
    for (i = 0; i < n; i++) { xi[i] = Q8(cx) + BELL_I[n - 1 - i][0] * 300; yi[i] = Q8(cy) + BELL_I[n - 1 - i][1] * 320; }
    art_curve(xo, yo, (uint32_t)n, art_c, 0, 1);
    art_ln(xo[n - 1], yo[n - 1], xi[0], yi[0], art_c, 0);
    art_curve(xi, yi, (uint32_t)n, art_c, 0, 1);
    art_hatch(art_c3, 3);
    /* the left half: the outside only */
    for (i = 0; i < n; i++) xo[i] = Q8(cx) - BELL_O[i][0] * 300;
    art_curve(xo, yo, (uint32_t)n, art_c, 0, 0);
    /* the (2,0) mode: the wall out on one side, in on the other, the more toward the lip (dashed) */
    for (i = 0; i < n; i++) {
        int32_t k = BELL_O[i][1] * d / 78;
        xo[i] = Q8(cx) + BELL_O[i][0] * 300 + k * 8;
        xi[i] = Q8(cx) - BELL_O[i][0] * 300 + k * 8;
    }
    art_curve(xo, yo, (uint32_t)n, art_c2, 3u | 2u << 8, 0);
    art_curve(xi, yo, (uint32_t)n, art_c2, 3u | 2u << 8, 0);
    /* the crown's canon, the clapper swinging */
    art_l(cx - 5, cy + 8, cx - 5, cy, art_c);
    art_l(cx + 5, cy + 8, cx + 5, cy, art_c);
    art_l(cx - 5, cy, cx + 5, cy, art_c);
    {
        int32_t sw = art_lv * art_sin(art_t * 0x0B000000u) / 32768;
        art_ln(Q8(cx), Q8(cy + 12), Q8(cx) + sw * 2, Q8(cy + 82), art_c2, 0);
        art_ring(Q8(cx) + sw * 2, Q8(cy + 87), 5, art_c2);
    }
    /* the partials: a stem plot on a log-frequency axis (40 Hz .. 12 kHz), the model's own */
    {
        const int32_t x0 = 118, x1 = 234, yb = PLOT_H - 16, ht = 84;
        uint32_t k, nn = rz_view.n > 16u ? 16u : rz_view.n;
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
}

/* ------------------------------------------------------- STRING and SYMP --- */
/* one mode n of a string from x0 to x1 at y: a8 sin(n pi u) (Q8 pixels) drawn, its envelope env8 dashed, its nodes */
static void art_mode(int32_t x0, int32_t x1, int32_t y, uint32_t n, int32_t a8, int32_t env8, uint16_t c, int nodes)
{
    int32_t i, px = Q8(x0), py = Q8(y), pe = py, pf = py, N = 48;
    for (i = 1; i <= N; i++) {
        uint32_t u = (uint32_t)i * n * (0x80000000u / 48u);
        int32_t s = art_sin(u), x = Q8(x0) + (x1 - x0) * 256 * i / N;
        int32_t yv = Q8(y) - a8 * s / 32768, ye = Q8(y) - env8 * s / 32768, yf = Q8(y) + env8 * s / 32768;
        art_ln(px, py, x, yv, c, 0);
        if (env8) {
            art_ln(px, pe, x, ye, art_c3, 2u | 2u << 8);
            art_ln(px, pf, x, yf, art_c3, 2u | 2u << 8);
        }
        px = x; py = yv; pe = ye; pf = yf;
    }
    if (nodes)
        for (i = 0; i <= (int32_t)n; i++)
            art_ring(Q8(x0) + (x1 - x0) * 256 * i / (int32_t)n, Q8(y), 1, art_c2);
}
static void art_bridge(int32_t x, int32_t y)
{
    art_l(x - 4, y + 5, x, y, art_c);
    art_l(x, y, x + 4, y + 5, art_c);
    art_l(x - 4, y + 5, x + 4, y + 5, art_c);
}
static void art_string_model(void)
{
    const int32_t x0 = 22, x1 = 228, yt = 14;
    uint32_t k, pp = (sys.fx_pos & 127u) * (0x80000000u / 128u) + 0x01000000u;
    int32_t xp = x0 + (x1 - x0) * (int32_t)(sys.fx_pos & 127u) / 127, amp[4], sum[49], i;
    int32_t mx = 1;
    for (k = 0; k < 4u; k++) {                                   /* each mode's weight: sin(n pi p) / n^2 */
        amp[k] = art_sin((k + 1u) * pp) / (int32_t)((k + 1u) * (k + 1u));
        mx = amp[k] > mx ? amp[k] : -amp[k] > mx ? -amp[k] : mx;
    }
    for (k = 0; k < 4u; k++)                                     /* (the largest drawn full height) */
        amp[k] = amp[k] * 32767 / mx;
    for (k = 0; k < 4u; k++) {                                   /* the modes, each at n times the first */
        int32_t y = 42 + (int32_t)k * 22, env = 8 * 256 * (amp[k] < 0 ? -amp[k] : amp[k]) / 32768;
        int32_t a = env * art_lv / 256 * art_cos((k + 1u) * art_t * 0x26000000u) / 32768;
        char lb[2] = {(char)('1' + k), 0};
        art_mode(x0, x1, y, k + 1u, amp[k] < 0 ? -a : a, env, art_c2, 1);
        cv_text(6, y - 8, &AF_S, lb, art_c3);
    }
    for (i = 0; i <= 48; i++) {                                  /* the string: their sum */
        uint32_t u = (uint32_t)i * (0x80000000u / 48u);
        int32_t d = 0;
        for (k = 0; k < 4u; k++)
            d += (int32_t)((((int64_t)amp[k] * art_cos((k + 1u) * art_t * 0x26000000u)) >> 15) * art_sin((k + 1u) * u) >> 15);
        sum[i] = Q8(yt) - d * 10 * art_lv / 32768;
    }
    for (i = 1; i <= 48; i++)
        art_ln(Q8(x0) + (x1 - x0) * 256 * (i - 1) / 48, sum[i - 1], Q8(x0) + (x1 - x0) * 256 * i / 48, sum[i], art_c, 0);
    art_bridge(x0, yt);
    art_bridge(x1, yt);
    art_ln(Q8(xp), Q8(yt + 6), Q8(xp), Q8(PLOT_H - 4), art_c3, 1u | 3u << 8);   /* where it is plucked */
}
static void art_sitar(void)
{
    const int32_t x0 = 30, x1 = 228, yt = 14;
    uint32_t k, nn = rz_view.n > 12u ? 12u : rz_view.n, fmin = 0xFFFFFFFFu;
    int32_t a = art_lv * 6 * art_cos(art_t * 0x26000000u) / 32768, row = 0, xs[4], ys[4];
    /* the played string over its broad curved bridge (the jawari: the string grazing it), bridge hatched */
    art_mode(x0, x1, yt, 1, a, 0, art_c, 0);
    art_mode(x0, x1, yt, 2, a / 3, 0, art_c2, 0);
    xs[0] = Q8(x0 - 14); ys[0] = Q8(yt + 8); xs[1] = Q8(x0 - 6); ys[1] = Q8(yt + 1); xs[2] = Q8(x0 + 2);
    ys[2] = Q8(yt); xs[3] = Q8(x0 + 2); ys[3] = Q8(yt + 8);
    art_section(xs, ys, 4, art_c, 3);
    art_ln(Q8(x1), Q8(yt - 4), Q8(x1), Q8(PLOT_H - 4), art_c3, 0);   /* the nut */
    /* every resonance as a string of its length (1 / f), swinging with its level; the sympathetic ones bright */
    for (k = 0; k < nn; k++)
        if (rz_view.f[k] && rz_view.f[k] < fmin)
            fmin = rz_view.f[k];
    for (k = 0; k < nn && sys.fx_mix; k++) {
        uint32_t f = rz_view.f[k];
        int32_t len, y, lv, sym = rz_view.a[k] < 0;
        if (!f)
            continue;
        len = (int32_t)((fmin >> 12) * (uint32_t)(x1 - x0) / ((f >> 12) | 1u));
        len = len < 14 ? 14 : len;
        y = 32 + row * 8;
        if (y > PLOT_H - 4)
            break;
        row++;
        lv = sym ? art_lv * art_lv / 64 : (int32_t)rz_view.a[k] * art_lv / 8192;   /* Q8 pixels: 4 at most */
        art_mode(x1 - len, x1, y, 1, lv * art_cos(art_t * (0x26000000u + k * 0x05000000u)) / 32768, 0,
                 sym ? art_c : art_c2, 0);
        art_l(x1 - len, y - 2, x1 - len, y + 2, art_c3);
    }
}

/* ----------------------------------------------------------------- THROAT --- */
/* the human tract in midsagittal section, pixels, the face to the right; the tongue per vowel (U O A E I: the order
 * of WG_SHAPE), tip to root, as in the phonetics books' X-ray tracings (simplified) */
static const int8_t TONGUE[WG_NV][7][2] = {
    {{94, 63}, {88, 62}, {78, 55}, {66, 49}, {56, 55}, {50, 72}, {51, 90}},   /* U: high, back */
    {{94, 64}, {88, 63}, {78, 59}, {66, 54}, {56, 61}, {49, 75}, {50, 90}},   /* O */
    {{94, 64}, {87, 65}, {77, 65}, {66, 63}, {55, 67}, {47, 79}, {49, 90}},   /* A: low, the root back */
    {{95, 61}, {89, 55}, {79, 53}, {67, 56}, {58, 64}, {53, 78}, {51, 90}},   /* E */
    {{95, 60}, {90, 52}, {80, 48}, {68, 51}, {59, 62}, {54, 77}, {52, 90}},   /* I: high, front */
};
static int32_t art_mz;                           /* the muzzle: Q8 stretch of the head in front of the velum */
static uint32_t art_jaw;                         /* the jaw's opening, a phase */
#define ART_TMJ_X 50
#define ART_TMJ_Y 40
/* a template point to the canvas (Q8): the muzzle stretched in front of x 66, then (jaw: 1 half, 2 all) turned about
 * the joint */
static void art_xf(int32_t X, int32_t Y, int jaw, int32_t *ox, int32_t *oy)   /* X, Y: Q8 template */
{
    if (X > Q8(66))
        X = Q8(66) + (X - Q8(66)) * (256 + art_mz) / 256;
    if (jaw) {
        uint32_t a = art_jaw * (uint32_t)jaw / 2u;
        int32_t s = art_sin(a), c = art_cos(a), dx = X - Q8(ART_TMJ_X), dy = Y - Q8(ART_TMJ_Y);
        X = Q8(ART_TMJ_X) + (int32_t)(((int64_t)dx * c - (int64_t)dy * s) >> 15);
        Y = Q8(ART_TMJ_Y) + (int32_t)(((int64_t)dx * s + (int64_t)dy * c) >> 15);
    }
    *ox = X;
    *oy = Y;
}
static void art_tpath(const int8_t (*p)[2], uint32_t n, int jaw, uint16_t c, uint32_t dash, int keep)
{
    int32_t x[12], y[12];
    uint32_t i;
    for (i = 0; i < n && i < 12u; i++)
        art_xf(Q8(p[i][0]), Q8(p[i][1]), jaw, &x[i], &y[i]);
    art_curve(x, y, n, c, dash, keep);
}
static const int8_t T_PHAR[][2] = {{40, 22}, {40, 50}, {39, 80}, {41, 112}, {42, 124}};          /* back wall */
static const int8_t T_NASAL[][2] = {{40, 22}, {52, 16}, {74, 14}, {92, 12}, {100, 10}};           /* nasal roof */
static const int8_t T_NOSE[][2] = {{100, 2}, {104, 20}, {113, 37}, {108, 43}, {104, 44}};         /* the nose */
static const int8_t T_PAL_O[][2] = {{58, 40}, {72, 38}, {86, 40}, {96, 46}, {98, 50}};            /* palate, oral */
static const int8_t T_PAL_N[][2] = {{98, 50}, {94, 41}, {86, 36}, {72, 34}, {58, 36}};            /* palate, nasal */
static const int8_t T_VEL_A[][2] = {{58, 40}, {52, 44}, {47, 51}, {45, 56}};                     /* the velum */
static const int8_t T_VEL_B[][2] = {{58, 36}, {50, 40}, {45, 47}, {45, 56}};
static const int8_t T_ULIP[][2] = {{104, 44}, {106, 48}, {107, 53}, {103, 56}};                  /* upper lip */
static const int8_t T_LLIP[][2] = {{103, 59}, {107, 61}, {107, 66}, {102, 70}, {104, 78},       /* lower lip, chin, */
                                   {98, 84}, {84, 86}, {70, 88}, {62, 92}};                       /* under the jaw */
static const int8_t T_UTEETH[][2] = {{96, 48}, {98, 56}, {95, 56}, {94, 49}};                     /* upper incisor */
static const int8_t T_LTEETH[][2] = {{95, 68}, {97, 60}, {94, 60}, {92, 67}};                     /* lower incisor */
static const int8_t T_CANINE[][2] = {{86, 44}, {89, 61}, {85, 50}};                               /* (carnivores) */
static const int8_t T_EPI[][2] = {{51, 90}, {47, 84}, {46, 81}, {50, 92}, {54, 97}};              /* epiglottis */
static const int8_t T_LAR[][2] = {{54, 97}, {58, 100}, {60, 108}, {58, 124}};                    /* larynx front */
static const int8_t T_NECK[][2] = {{62, 92}, {68, 104}, {70, 124}};                               /* the throat */
static const int8_t T_CONCHA[3][3][2] = {{{56, 19}, {68, 19}, {80, 21}}, {{58, 25}, {72, 25}, {86, 27}},
                                         {{60, 30}, {74, 30}, {88, 32}}};
/* per animal: the muzzle (Q8 stretch), canines, no upper incisors (the cow's dental pad), the trunk */
static const struct { uint8_t mz, canine, pad, trunk; } ANI_ART[WA_COUNT] = {
    {40, 1, 0, 0}, {50, 1, 0, 0}, {0, 0, 0, 0}, {150, 1, 0, 0}, {170, 1, 0, 0}, {180, 0, 1, 0}, {110, 1, 0, 0},
    {60, 0, 0, 1},
};
static void art_throat(void)
{
    uint32_t an = (sys.fx_struct & 127u) * WA_COUNT / 128u, i;
    uint32_t vq = (sys.fx_pos & 127u) * (WG_NV - 1u) * 256u / 127u, v0 = vq >> 8, v1 = v0 + 1u < WG_NV ? v0 + 1u : v0;
    int32_t vf = (int32_t)(vq & 255u), x[12], y[12];
    art_mz = ANI_ART[an].mz;
    art_jaw = (uint32_t)art_lv * WG_ANI[an].wah * 4000u;         /* (up to ~12 degrees) */
    for (i = 0; i < 6u; i++) {                                   /* the cervical vertebrae: bone, hatched */
        int32_t yt = 30 + (int32_t)i * 15;
        x[0] = Q8(19); y[0] = Q8(yt + 2); x[1] = Q8(21); y[1] = Q8(yt); x[2] = Q8(32); y[2] = Q8(yt);
        x[3] = Q8(34); y[3] = Q8(yt + 2); x[4] = Q8(34); y[4] = Q8(yt + 10); x[5] = Q8(32); y[5] = Q8(yt + 12);
        x[6] = Q8(21); y[6] = Q8(yt + 12); x[7] = Q8(19); y[7] = Q8(yt + 10);
        art_section(x, y, 8, art_c2, 3);
    }
    art_tpath(T_PHAR, 5, 0, art_c, 0, 0);                        /* the pharynx's back wall */
    art_tpath(T_NASAL, 5, 0, art_c2, 0, 0);                      /* the nasal cavity, the nose, the conchae */
    art_tpath(T_NOSE, 5, 0, art_c, 0, 0);
    for (i = 0; i < 3u; i++)
        art_tpath(T_CONCHA[i], 3, 0, art_c3, 0, 0);
    art_bn = 0;                                                  /* the hard palate: bone, hatched */
    art_tpath(T_PAL_O, 5, 0, art_c, 0, 1);
    art_tpath(T_PAL_N, 5, 0, art_c, 0, 1);
    art_hatch(art_c3, 3);
    art_tpath(T_VEL_A, 4, 0, art_c, 0, 0);                       /* the velum hanging behind it */
    art_tpath(T_VEL_B, 4, 0, art_c, 0, 0);
    if (!ANI_ART[an].pad)                                        /* the cow: no upper incisors, its dental pad */
        art_tpath(T_UTEETH, 4, 0, art_c, 0, 0);
    else
        art_tpath(T_UTEETH, 2, 0, art_c, 0, 0);
    if (ANI_ART[an].canine)
        art_tpath(T_CANINE, 3, 0, art_c2, 0, 0);
    art_tpath(T_ULIP, 4, 0, art_c, 0, 0);
    art_tpath(T_LTEETH, 4, 2, art_c, 0, 0);                      /* the jaw, turned open with the level */
    art_tpath(T_LLIP, 9, 2, art_c, 0, 0);
    art_tpath(T_NECK, 3, 1, art_c, 0, 0);
    for (i = 0; i < 7u; i++)                                     /* the tongue at the VOWEL, turning with the jaw */
        art_xf(TONGUE[v0][i][0] * 256 + (TONGUE[v1][i][0] - TONGUE[v0][i][0]) * vf,
               TONGUE[v0][i][1] * 256 + (TONGUE[v1][i][1] - TONGUE[v0][i][1]) * vf, i < 5u ? 1 : 0, &x[i], &y[i]);
    art_curve(x, y, 7, art_c, 0, 0);
    art_tpath(T_EPI, 5, 0, art_c, 0, 0);
    art_tpath(T_LAR, 4, 0, art_c, 0, 0);
    {   /* the vocal folds: two wedges across the larynx, the gap beating with the voice */
        int32_t g = art_lv * (art_sin(art_t * 0x4C000000u) + 32768) / 65536 * 3, yv = Q8(106);
        art_ln(Q8(42), yv - Q8(3), Q8(49) - g, yv, art_c, 0);
        art_ln(Q8(42), yv + Q8(3), Q8(49) - g, yv, art_c, 0);
        art_ln(Q8(59), yv - Q8(3), Q8(51) + g, yv, art_c, 0);
        art_ln(Q8(59), yv + Q8(3), Q8(51) + g, yv, art_c, 0);
        for (i = 0; i < 3u; i++)                                 /* the trachea's rings below */
            art_ln(Q8(43), Q8(113 + i * 4), Q8(58), Q8(113 + i * 4), art_c3, 2u | 1u << 8);
    }
    if (ANI_ART[an].trunk) {                                     /* the trunk: the nose drawn out, hanging */
        int32_t sw = art_lv * art_sin(art_t * 0x06000000u) / 32768, ex, ey;
        art_xf(Q8(113), Q8(37), 0, &ex, &ey);
        x[0] = ex; y[0] = ey; x[1] = ex + Q8(9); y[1] = ey + Q8(26); x[2] = ex + Q8(6) + sw * 4; y[2] = ey + Q8(58);
        x[3] = ex + sw * 8; y[3] = ey + Q8(84);
        art_curve(x, y, 4, art_c, 0, 0);
        art_xf(Q8(104), Q8(44), 0, &ex, &ey);
        x[0] = ex; y[0] = ey; x[1] = ex + Q8(6); y[1] = ey + Q8(24); x[2] = ex + Q8(2) + sw * 4; y[2] = ey + Q8(52);
        x[3] = ex - Q8(4) + sw * 8; y[3] = ey + Q8(76);
        art_curve(x, y, 4, art_c, 0, 0);
        art_ln(x[3], y[3], ex + Q8(9) + sw * 8 - Q8(9), y[3] + Q8(1), art_c, 0);
    }
    if (art_lv > 12) {                                           /* the air: up from the glottis, out of the mouth */
        uint32_t nd = (uint32_t)art_lv / 20u;
        for (i = 0; i < nd; i++) {
            uint32_t ph = (art_t * 5u + i * 37u) % 100u;
            int32_t px, py;
            if (ph < 50u)
                art_xf(Q8(48 + (int32_t)(i % 5u)), Q8(102 - (int32_t)ph), 0, &px, &py);
            else
                art_xf(Q8(50 + (int32_t)(ph - 50u)), Q8(56 + (int32_t)(i % 4u) - 2), 1, &px, &py);
            art_px(px >> 8, py >> 8, art_c, 200);
        }
    }
    {   /* beside it: the area function A(x), the model's tube in section, glottis (G) to lips (L) */
        const int32_t x0 = 170, x1 = 234, yc = 60;
        int32_t k, prev = 0;
        art_ln(Q8(x0 - 4), Q8(yc), Q8(x1 + 4), Q8(yc), art_c3, 6u | 3u << 8);
        for (k = 0; k < 16; k++) {
            int32_t a0 = WG_SHAPE[v0][k] + (((int32_t)WG_SHAPE[v1][k] - WG_SHAPE[v0][k]) * vf >> 8);
            int32_t h = 1 + (int32_t)art_isqrt((uint32_t)a0 * 24u) / 2, xa = x0 + (x1 - x0) * k / 16;
            int32_t xb = x0 + (x1 - x0) * (k + 1) / 16;
            if (k == 15)
                h += art_lv * (int32_t)WG_ANI[an].wah / 4096;    /* (the lips opening with the jaw) */
            if (k) {
                art_l(xa, yc - prev, xa, yc - h, art_c);
                art_l(xa, yc + prev, xa, yc + h, art_c);
            }
            art_l(xa, yc - h, xb, yc - h, art_c);
            art_l(xa, yc + h, xb, yc + h, art_c);
            prev = h;
        }
        cv_text(x0, 8, &AF_S, "A(x)", art_c3);
        cv_text(x0 - 2, yc + 30, &AF_S, "G", art_c3);
        cv_text(x1 - 6, yc + 30, &AF_S, "L", art_c3);
        cv_text(x0, PLOT_H - 15, &AF_S, WG_ANIMAL[an], art_c2);
    }
}

/* ------------------------------------------------------------------ FLUTE --- */
static void art_flute(void)
{
    const int32_t x0 = 14, x1 = 232, yt = 40, yb = 58, hx = 56, ym = (yt + yb) / 2, gy = 98;
    uint32_t nh = 1u + (sys.fx_struct & 127u) * 3u / 128u, i;
    int32_t px = 0, py = 0, x[4], y[4];
    /* the bore's walls in section (hatched), the embouchure hole in the top one, the cork at the left */
    x[0] = Q8(x0); y[0] = Q8(yt - 3); x[1] = Q8(hx - 5); y[1] = y[0]; x[2] = x[1]; y[2] = Q8(yt); x[3] = x[0]; y[3] = y[2];
    art_section(x, y, 4, art_c, 3);
    x[0] = Q8(hx + 5); x[1] = Q8(x1); x[2] = x[1]; x[3] = x[0];
    art_section(x, y, 4, art_c, 3);
    x[0] = Q8(x0); y[0] = Q8(yb); x[1] = Q8(x1); y[1] = y[0]; x[2] = x[1]; y[2] = Q8(yb + 3); x[3] = x[0]; y[3] = y[2];
    art_section(x, y, 4, art_c, 3);
    x[0] = Q8(x0); y[0] = Q8(yt); x[1] = Q8(x0 + 7); y[1] = y[0]; x[2] = x[1]; y[2] = Q8(yb); x[3] = x[0]; y[3] = y[2];
    art_section(x, y, 4, art_c2, 2);
    /* the lips (in section) over the hole, the jet from them to the edge, its wave growing along it */
    x[0] = Q8(hx - 20); y[0] = Q8(yt - 24); x[1] = Q8(hx - 13); y[1] = Q8(yt - 21);
    x[2] = Q8(hx - 12); y[2] = Q8(yt - 17); x[3] = Q8(hx - 18); y[3] = Q8(yt - 15);
    art_curve(x, y, 4, art_c, 0, 0);
    x[0] = Q8(hx - 18); y[0] = Q8(yt - 13); x[1] = Q8(hx - 12); y[1] = Q8(yt - 11);
    x[2] = Q8(hx - 13); y[2] = Q8(yt - 7); x[3] = Q8(hx - 20); y[3] = Q8(yt - 4);
    art_curve(x, y, 4, art_c, 0, 0);
    for (i = 0; i <= 20u; i++) {
        int32_t xx = Q8(hx - 12) + Q8(17) * (int32_t)i / 20, yy = Q8(yt - 14) + Q8(14) * (int32_t)i / 20;
        yy += art_lv * (int32_t)i * art_sin(art_t * 0x50000000u - i * 0x16000000u) / (32768 * 20) * 5;
        if (i)
            art_ln(px, py, xx, yy, art_c, 0);
        px = xx;
        py = yy;
    }
    if (art_lv > 8) {                                            /* the breath noise: dust along the jet */
        uint32_t n = (uint32_t)art_lv * (sys.fx_pos & 127u) / 1024u, r = art_t * 2654435761u;
        for (i = 0; i < n; i++) {
            uint32_t u;
            r = r * 1664525u + 1013904223u;
            u = r >> 28;
            art_px(hx - 12 + (int32_t)u, yt - 14 + (int32_t)u * 14 / 16 + (int32_t)((r >> 24) & 7u) - 3, art_c2, 180);
        }
    }
    /* inside: the air's displacement (dashed); below: the pressure along the bore, its envelope, its nodes */
    art_l(hx, gy, x1, gy, art_c3);
    art_l(hx, gy - 18, hx, gy + 18, art_c3);
    cv_text(hx - 12, gy - 20, &AF_S, "P", art_c3);
    {
        int32_t cw = art_cos(art_t * 0x30000000u), e = 16 * 256;
        int32_t pe = Q8(gy), pf = Q8(gy), pv = Q8(gy), pu = Q8(ym);
        for (i = 0; i <= 48u; i++) {
            int32_t xx = Q8(hx) + (Q8(x1) - Q8(hx)) * (int32_t)i / 48;
            uint32_t u = i * nh * (0x80000000u / 48u);
            int32_t s = art_sin(u), yv = Q8(gy) - e * art_lv / 256 * cw / 32768 * s / 32768;
            int32_t ye = Q8(gy) - e * s / 32768, yf = Q8(gy) + e * s / 32768;
            int32_t yu = Q8(ym) - 6 * art_lv * cw / 32768 * art_cos(u) / 32768;
            if (i) {
                art_ln(px, pv, xx, yv, art_c, 0);
                art_ln(px, pe, xx, ye, art_c3, 2u | 2u << 8);
                art_ln(px, pf, xx, yf, art_c3, 2u | 2u << 8);
                art_ln(px, pu, xx, yu, art_c2, 3u | 2u << 8);
            }
            px = xx; pv = yv; pe = ye; pf = yf; pu = yu;
        }
        for (i = 0; i <= nh; i++)                                /* the pressure nodes */
            art_ring(Q8(hx) + (Q8(x1) - Q8(hx)) * (int32_t)i / (int32_t)nh, Q8(gy), 1, art_c2);
    }
}

/* -------------------------------------------------------- SYRINX and HAWK --- */
static void art_syrinx(int hawk)
{
    const int32_t cx = 62, yj = 60;                  /* the fork */
    uint32_t i, rr = art_t * 2246822519u;
    int32_t beat = (int32_t)(sys.fx_pos & 127u), x[6], y[6];
    uint32_t ph1 = art_t * 0x3C000000u, ph2 = art_t * (0x3C000000u + (uint32_t)beat * 0x00300000u);
    int32_t g1 = art_lv * (art_sin(ph1) + 32768) / 65536 * 3, g2 = art_lv * (art_sin(ph2) + 32768) / 65536 * 3;
    if (hawk && art_lv > 16) {                       /* (the rasp: the labia shaken) */
        g1 += (int32_t)((rr >> 28) & 7u) * beat * 2;
        g2 += (int32_t)((rr >> 24) & 7u) * beat * 2;
    }
    art_l(cx - 8, 0, cx - 8, yj - 10, art_c);        /* the trachea, its cartilage rings */
    art_l(cx + 8, 0, cx + 8, yj - 10, art_c);
    for (i = 0; i < 9u; i++)
        art_ln(Q8(cx - 8), Q8(4 + i * 5), Q8(cx + 8), Q8(5 + i * 5), art_c3, 0);
    /* the syrinx: the tympanum widening into the fork, the bronchi; the pessulus between them */
    x[0] = Q8(cx - 8); y[0] = Q8(yj - 10); x[1] = Q8(cx - 13); y[1] = Q8(yj - 2); x[2] = Q8(cx - 14); y[2] = Q8(yj + 6);
    x[3] = Q8(cx - 30); y[3] = Q8(PLOT_H - 2);
    art_curve(x, y, 4, art_c, 0, 0);
    for (i = 0; i < 4u; i++) x[i] = Q8(2 * cx) - x[i];
    art_curve(x, y, 4, art_c, 0, 0);
    x[0] = Q8(cx - 3); y[0] = Q8(PLOT_H - 2); x[1] = Q8(cx - 2); y[1] = Q8(yj + 14); x[2] = Q8(cx); y[2] = Q8(yj + 4);
    art_curve(x, y, 3, art_c, 0, 0);
    for (i = 0; i < 3u; i++) x[i] = Q8(2 * cx) - x[i];
    art_curve(x, y, 3, art_c, 0, 0);
    art_ring(Q8(cx), Q8(yj + 3), 1, art_c);
    for (i = 0; i < 4u; i++) {                                   /* the bronchial half rings */
        int32_t yy = yj + 20 + (int32_t)i * 9, o = (int32_t)i * 4;
        art_l(cx - 16 - o, yy, cx - 5 - o / 3, yy + 1, art_c3);
        art_l(cx + 5 + o / 3, yy + 1, cx + 16 + o, yy, art_c3);
    }
    for (i = 0; i < 2u; i++) {                                   /* the labia, lateral and medial, each side */
        int32_t s = i ? 1 : -1, g = i ? g2 : g1, xo = cx + s * 13, xm = cx + s * 3;
        x[0] = Q8(xo); y[0] = Q8(yj); x[1] = Q8(xo) - s * (Q8(4) + g / 2); y[1] = Q8(yj + 6);
        x[2] = Q8(xo + s); y[2] = Q8(yj + 12);
        art_curve(x, y, 3, art_c, 0, 0);
        x[0] = Q8(xm); y[0] = Q8(yj + 2); x[1] = Q8(xm) + s * (Q8(3) + g / 2); y[1] = Q8(yj + 8);
        x[2] = Q8(xm + s); y[2] = Q8(yj + 14);
        art_curve(x, y, 3, art_c, 0, 0);
    }
    art_ln(Q8(cx - 9), Q8(30), Q8(cx - 20), Q8(yj + 16), art_c3, 3u | 2u << 8);   /* the syringeal muscles */
    art_ln(Q8(cx + 9), Q8(30), Q8(cx + 20), Q8(yj + 16), art_c3, 3u | 2u << 8);
    if (art_lv > 12) {                                           /* the air: up the bronchi, through the trachea */
        uint32_t nd = (uint32_t)art_lv / 20u;
        for (i = 0; i < nd; i++) {
            uint32_t ph = (art_t * 4u + i * 29u) % 120u;
            int32_t sd = (i & 1u) ? 1 : -1, px, py;
            if (ph < 50u) { py = PLOT_H - 2 - (int32_t)ph; px = cx + sd * (18 - (int32_t)ph / 4); }
            else { py = yj - 12 - (int32_t)(ph - 50u); px = cx + sd * 3; }
            art_px(px, py, art_c, 220);
        }
    }
    cv_text(cx - 40, 4, &AF_S, "L", art_c3);
    cv_text(cx + 32, 4, &AF_S, "R", art_c3);
    {   /* beside it: the two voices' traces */
        const int32_t x0 = 134, x1 = 234;
        for (i = 0; i < 2u; i++) {
            int32_t yc = 32 + (int32_t)i * 52, k, ppx = Q8(x0), ppy = Q8(yc);
            uint32_t fr = 0x10000000u + (i ? (uint32_t)beat * 0x00080000u : 0u);
            art_l(x0, yc, x1, yc, art_c3);
            cv_text(x0 - 12, yc - 8, &AF_S, i ? "R" : "L", art_c3);
            for (k = 1; k <= 60; k++) {
                int32_t xx = Q8(x0) + (Q8(x1) - Q8(x0)) * k / 60;
                int32_t v = art_sin((uint32_t)k * fr + art_t * 0x09000000u * (i + 1u)) / 128 * art_lv / 256 * 18;
                if (hawk && art_lv > 16)
                    v += ((int32_t)((((uint32_t)k * 2654435761u + art_t * 40503u) >> 24) & 255u) - 128) * beat / 16;
                art_ln(ppx, ppy, xx, Q8(yc) - v, art_c, 0);
                ppx = xx;
                ppy = Q8(yc) - v;
            }
        }
    }
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
