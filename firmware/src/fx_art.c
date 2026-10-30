/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX page's pictures (ui.c): each model drawn as a simplified line drawing, as in a medical or a physics book,
 * in the theme's orange, moving with the sound it makes. Included by ui.c (its canvas, its colours).
 *
 *   MODAL    a bell in profile: its wall breathes with the level, the clapper swings, waves leave it
 *   SYMP     a sitar: the gourd, the neck, the played string and three sympathetic ones under it ringing along
 *   STRING   a string between two bridges, plucked at POSITION: its first modes, each at its own speed
 *   THROAT   the animal's head cut through (sagittal), the vocal tract inside: its width along the tube the model's
 *            own area function (the VOWEL), the vocal folds at the bottom opening and closing, the jaw with the level
 *   FLUTE    the bore cut through, the lips over the embouchure, the jet bending across the edge, the standing wave
 *            inside (one, two or three half waves: the JET's overblowing), the breath as dust
 *   SYRINX   a songbird: head, open beak, the trachea down the neck to the syrinx where it forks into the bronchi,
 *            its two membranes each beating with its own voice
 *   HAWK     the same in a hawk's head (the hooked beak, the brow), the membranes rasping
 *
 * Everything is drawn from the level of the wet output (rz_view.wave) and the FX knobs; nothing here touches the
 * audio. Integer only (np_sin: a phase, Q32 a turn -> Q15). */

static uint32_t art_t;                           /* frames drawn (the motion's clock) */
static int32_t art_lv;                           /* the output's level now, 0..256 (follows fast up, slowly down) */
static uint16_t art_c;                           /* the line colour (the theme's orange; dim when MIX is off) */

static void art_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) { cv_line_t(x0, y0, x1, y1, art_c, 2); }
static void art_thin(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { cv_line(x0, y0, x1, y1, c); }
static int32_t art_sin(uint32_t ph) { return np_sin(ph); }               /* Q15 */
static int32_t art_cos(uint32_t ph) { return np_sin(ph + 0x40000000u); }
#define ART_DEG(d) ((uint32_t)(int32_t)(d) * 11930465u)   /* degrees -> a phase (2^32 / 360) */

/* a polyline of n points (x, y pairs) at (ox, oy), mirrored in x when mx < 0 */
static void art_poly(const int8_t *p, uint32_t n, int32_t ox, int32_t oy, int32_t mx)
{
    uint32_t i;
    for (i = 1; i < n; i++)
        art_line(ox + mx * p[2 * i - 2], oy + p[2 * i - 1], ox + mx * p[2 * i], oy + p[2 * i + 1]);
}
/* an arc around (cx, cy), radius r, from angle a0 to a1 (degrees, 0 right, 90 down), in steps */
static void art_arc(int32_t cx, int32_t cy, int32_t r, int32_t a0, int32_t a1, uint16_t c, int thick)
{
    int32_t a, st = (a1 - a0) / 12, px = 0, py = 0;
    if (!st)
        st = a1 > a0 ? 1 : -1;
    for (a = a0;; a += st) {
        int32_t x, y;
        if ((st > 0 && a > a1) || (st < 0 && a < a1))
            a = a1;
        x = cx + r * art_cos(ART_DEG(a)) / 32768;
        y = cy + r * art_sin(ART_DEG(a)) / 32768;
        if (a != a0) {
            if (thick)
                cv_line_t(px, py, x, y, c, 2);
            else
                cv_line(px, py, x, y, c);
        }
        px = x;
        py = y;
        if (a == a1)
            break;
    }
}
static void art_dot(int32_t x, int32_t y) { cv_rect(x - 1, y - 1, 3, 3, art_c); }
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
/* sound leaving at (x, y) to the right: three arcs moving out, fading with the level and the distance */
static void art_waves(int32_t x, int32_t y, int32_t spread, int32_t room)
{
    uint32_t k;
    if (art_lv < 8)
        return;
    for (k = 0; k < 3u; k++) {
        int32_t r = 6 + (int32_t)((art_t * 3u + k * (uint32_t)room / 3u) % (uint32_t)room);
        int32_t a = art_lv * (room - r) / room;
        art_arc(x - r / 2, y, r, -spread, spread, ux_mix(T_BG, T_THEME, clamp(a * 100 / 256, 0, 100)), 0);
    }
}

/* ------------------------------------------------------------------ MODAL --- */
static void art_bell(void)
{
    static const int8_t B[] = {0, 14, 9, 14, 15, 18, 18, 28, 19, 42, 21, 56, 26, 68, 34, 78, 40, 84, 34, 87, 0, 85};
    int8_t p[sizeof B];
    const int32_t cx = 70;
    int32_t w = art_lv * art_sin(art_t * 0x2E000000u) / 32768;   /* the wall's breathing (the (2,0) mode) */
    uint32_t i;
    for (i = 0; i < sizeof B / 2u; i++) {                    /* (more at the lip than at the crown) */
        p[2 * i] = (int8_t)(B[2 * i] + B[2 * i] * w * B[2 * i + 1] / (256 * 85 * 6));
        p[2 * i + 1] = B[2 * i + 1];
    }
    art_poly(p, sizeof B / 2u, cx, 4, 1);
    art_poly(p, sizeof B / 2u, cx, 4, -1);
    art_arc(cx, 12, 6, 180, 360, art_c, 1);                  /* the loop it hangs from */
    {   /* the clapper: swinging with the level */
        int32_t sw = art_lv * art_sin(art_t * 0x0C000000u) / (32768 * 24);
        art_line(cx, 22, cx + sw, 76);
        art_arc(cx + sw, 80, 4, 0, 360, art_c, 1);
    }
    for (i = 0; i < 3u; i++)                                 /* the nodal circles of the struck modes, dim */
        art_thin(cx - 14 - (int32_t)i * 4, 40 + (int32_t)i * 14, cx + 14 + (int32_t)i * 4, 40 + (int32_t)i * 14,
                 ux_mix(T_BG, T_THEME, 24));
    art_waves(cx + 52, 60, 40, 120);
}

/* ------------------------------------------------------- STRING and SYMP --- */
/* a string from (x0, y) to (x1, y), plucked at p (0..127), its first four modes at amplitude a (pixels) */
static void art_string(int32_t x0, int32_t x1, int32_t y, uint32_t p, int32_t a, uint32_t speed, uint16_t c, int thick)
{
    int32_t i, px = x0, py = y, n = 40;
    int32_t c4[4];
    uint32_t k, pp = (p & 127u) * (0x80000000u / 128u) + 0x01000000u;
    for (k = 0; k < 4u; k++)                                 /* each mode: sin(k pi p) / k^2, at its own speed */
        c4[k] = (int32_t)(((int64_t)art_sin((k + 1u) * pp) * art_cos((k + 1u) * art_t * speed)) >> 15) /
                (int32_t)((k + 1u) * (k + 1u));
    for (i = 1; i <= n; i++) {
        int32_t x = x0 + (x1 - x0) * i / n, d = 0;
        uint32_t u = (uint32_t)((int64_t)i * 0x80000000u / n);
        for (k = 0; k < 4u; k++)
            d += (int32_t)(((int64_t)c4[k] * art_sin((k + 1u) * u)) >> 15);
        d = d * a / 32768;
        if (thick)
            cv_line_t(px, py, x, y + d, c, 2);
        else
            cv_line(px, py, x, y + d, c);
        px = x;
        py = y + d;
    }
}
static void art_bridge(int32_t x, int32_t y)
{
    art_line(x - 5, y + 9, x, y);
    art_line(x, y, x + 5, y + 9);
    art_line(x - 7, y + 9, x + 7, y + 9);
}
static void art_string_model(void)
{
    const int32_t y = 58;
    int32_t xp = 20 + 200 * (int32_t)(sys.fx_pos & 127u) / 127;
    art_bridge(20, y);
    art_bridge(220, y);
    art_string(20, 220, y, sys.fx_pos, art_lv * 22 / 256, 0x2A000000u, art_c, 1);
    art_thin(xp, y + 16, xp, y + 22, ux_mix(T_BG, T_THEME, 55));   /* where it is plucked */
    cv_text(xp - 12, y + 26, &AF_S, "PLUCK", T_DIM);
    art_thin(20, y + 46, 220, y + 46, T_LINE);                        /* the soundboard under it */
}
static void art_sitar(void)
{
    static const int8_t G[] = {30, -2, 22, -18, 6, -26, -10, -24, -24, -12, -28, 4, -22, 20, -6, 28, 12, 26,
                               26, 16, 31, 4, 30, -2};
    const int32_t gx = 36, gy = 70, y = 58, x1 = 228;
    uint32_t k;
    int32_t lag = art_lv * art_lv / 256;                     /* (the sympathetic strings answer later, softer) */
    art_poly(G, sizeof G / 2u, gx, gy, 1);                   /* the gourd */
    art_line(gx + 24, y - 6, x1, y - 6);                     /* the neck */
    art_line(gx + 26, y + 10, x1, y + 10);
    art_line(x1, y - 6, x1, y + 10);
    for (k = 0; k < 7u; k++)                                 /* the frets (curved, as a sitar's) */
        art_arc(96 + (int32_t)k * 18, y + 2, 7, 200, 340, ux_mix(T_BG, T_THEME, 35), 0);
    art_bridge(gx, y - 6);
    art_string(gx, x1, y - 6, sys.fx_pos, art_lv * 14 / 256, 0x2A000000u, art_c, 1);
    for (k = 0; k < 3u; k++)                                 /* the sympathetic strings, each at its harmonic */
        art_string(gx + 18, x1 - 30 - (int32_t)k * 16, y + 16 + (int32_t)k * 5, 64u >> k, lag * 5 / 256,
                   0x2A000000u * (k + 2u), ux_mix(T_BG, T_THEME, 60), 0);
}

/* ----------------------------------------------------------------- THROAT --- */
/* the head (a cut through it): the tube from the larynx up the pharynx and forward through the mouth, its width the
 * area function at the VOWEL; the head's outline around it; per animal its snout, ears or horns, mane, trunk */
static void art_throat(void)
{
    uint32_t an = (sys.fx_struct & 127u) * WA_COUNT / 128u;
    uint32_t vq = (sys.fx_pos & 127u) * (WG_NV - 1u) * 256u / 127u, v0 = vq >> 8, v1 = v0 + 1u < WG_NV ? v0 + 1u : v0;
    int32_t vf = (int32_t)(vq & 255u);
    const int32_t ox = 58, yb = 120, R = 14, la = R * 157 / 100, N = 32;
    int32_t len = 92 + (int32_t)WG_ANI[an].n * (int32_t)WG_ANI[an].k * 2;   /* the tube on screen (a cat short) */
    int32_t lv_ = len * 42 / 100, lh, yo, lipx, open, i, pa[2] = {0, 0}, pb[2] = {0, 0};
    if (lv_ > 70)
        lv_ = 70;
    lh = len - lv_ - la;
    if (lh > 132)
        lh = 132;
    yo = yb - lv_ - R;                                       /* the mouth's line */
    lipx = ox + R + lh;
    open = art_lv * (int32_t)WG_ANI[an].wah / (256 * 14);    /* the jaw: the level, the animal's wah */
    for (i = 0; i <= N; i++) {                               /* the tube: glottis (bottom) to lips */
        int32_t s = (lv_ + la + lh) * i / N, px, py, nx, ny, ar, w, drop = 0;
        uint32_t q = (uint32_t)i * 15u * 256u / (uint32_t)N, j = q >> 8;
        int32_t f = (int32_t)(q & 255u), j1 = j + 1u < 16u ? (int32_t)j + 1 : (int32_t)j;
        int32_t a0 = WG_SHAPE[v0][j] + ((WG_SHAPE[v0][j1] - WG_SHAPE[v0][j]) * f >> 8);
        int32_t a1 = WG_SHAPE[v1][j] + ((WG_SHAPE[v1][j1] - WG_SHAPE[v1][j]) * f >> 8);
        ar = a0 + ((a1 - a0) * vf >> 8);
        w = 1 + (int32_t)art_isqrt((uint32_t)ar * 6u) / 2;   /* the half width: the area's square root */
        if (s < lv_) {                                       /* the pharynx: up */
            px = ox; py = yb - s; nx = 32768; ny = 0;
        } else if (s < lv_ + la) {                           /* the bend: a quarter circle */
            int32_t a = 180 + 90 * (s - lv_) / la;
            px = ox + R + R * art_cos(ART_DEG(a)) / 32768;
            py = yb - lv_ + R * art_sin(ART_DEG(a)) / 32768;
            nx = -art_cos(ART_DEG(a));
            ny = -art_sin(ART_DEG(a));
        } else {                                             /* the mouth: forward, the jaw dropping its floor */
            px = ox + R + (s - lv_ - la);
            py = yo; nx = 0; ny = 32768;
            drop = open * (px - ox - R) / (lh > 0 ? lh : 1);
        }
        {
            int32_t ax = px + nx * w / 32768, ay = py + ny * w / 32768 + drop;   /* the tongue's side */
            int32_t bx = px - nx * w / 32768, by = py - ny * w / 32768;          /* the back wall, the palate */
            if (i) {
                art_line(pa[0], pa[1], ax, ay);
                art_line(pb[0], pb[1], bx, by);
            }
            pa[0] = ax; pa[1] = ay; pb[0] = bx; pb[1] = by;
        }
    }
    {   /* the vocal folds: across the tube at the bottom, the gap opening and closing with the voice */
        int32_t g = 1 + art_lv * (art_sin(art_t * 0x4C000000u) + 32768) / (65536 * 48), y = yb - 8;
        art_line(ox - 7, y, ox - g, y + 3);
        art_line(ox + 7, y, ox + g, y + 3);
    }
    {   /* the head around it (a cut through the middle): neck, skull, brow, snout, lips, chin, jaw, throat */
        int32_t sn = lipx + 6, top = yo - 28, ch = yo + 18 + open;
        art_line(ox - 18, PLOT_H, ox - 18, yo + 2);                         /* the back of the neck */
        art_arc(ox + 12, yo + 2, 30, 180, 270, art_c, 1);                   /* the back of the skull */
        art_line(ox + 12, top, lipx - 26, top);                             /* the crown */
        art_line(lipx - 26, top, lipx - 10, top + 6);                       /* the brow */
        art_line(lipx - 10, top + 6, sn, yo - 9);                           /* the face down to the nose */
        art_line(sn, yo - 9, lipx + 1, yo - 4);                             /* the upper lip */
        art_line(lipx + 1, yo + 4 + open, sn - 1, yo + 8 + open);           /* the lower lip, with the jaw */
        art_line(sn - 1, yo + 8 + open, lipx - 6, ch);                      /* the chin */
        art_line(lipx - 6, ch, ox + 30, yo + 24 + open / 2);                /* the jaw */
        art_line(ox + 30, yo + 24 + open / 2, ox + 18, yo + 34);            /* the throat */
        art_line(ox + 18, yo + 34, ox + 18, PLOT_H);
        art_dot(lipx - 20, top + 9);                                        /* the eye */
        switch (an) {
        case WA_CAT: case WA_DOG: case WA_WOLF:              /* pointed ears */
            art_line(ox + 4, top + 4, ox + 12, top - 14);
            art_line(ox + 12, top - 14, ox + 20, top + 2);
            break;
        case WA_MONKEY:                                      /* a round ear on the side */
            art_arc(ox + 4, top + 14, 6, 0, 360, art_c, 1);
            break;
        case WA_COW:                                         /* a horn, an ear */
            art_arc(ox + 26, top + 2, 14, 190, 300, art_c, 1);
            art_line(ox + 32, top - 10, ox + 36, top - 18);
            art_line(ox + 2, top + 8, ox - 12, top + 14);
            break;
        case WA_LION: {                                      /* the mane: a zigzag behind the head */
            int32_t k;
            for (k = 0; k < 7; k++)
                art_line(ox - 22 - (k & 1) * 7, top + k * 9, ox - 22 - ((k + 1) & 1) * 7, top + 9 + k * 9);
            break;
        }
        case WA_ELEPHANT: {                                  /* the trunk hanging from the snout, swaying */
            int32_t sw = art_lv * art_sin(art_t * 0x08000000u) / (32768 * 32), y2 = PLOT_H - 4;
            art_line(sn, yo - 9, sn + 8 + sw, (yo + y2) / 2);
            art_line(sn + 8 + sw, (yo + y2) / 2, sn + 2 + sw * 2, y2);
            art_line(sn - 1, yo + 8 + open, sn + 1 + sw, (yo + y2) / 2 + 2);
            art_line(sn + 1 + sw, (yo + y2) / 2 + 2, sn - 5 + sw * 2, y2);
            art_line(sn - 5 + sw * 2, y2, sn + 2 + sw * 2, y2);
            art_line(ox + 10, top + 6, ox - 16, top + 30);   /* the ear */
            break;
        }
        default:                                             /* the human: a nose */
            art_line(lipx - 10, top + 6, sn + 4, yo - 12);
            art_line(sn + 4, yo - 12, sn, yo - 9);
            break;
        }
    }
    art_waves(lipx + 22, yo + 2, 30, 50);
}

/* ------------------------------------------------------------------ FLUTE --- */
static void art_flute(void)
{
    const int32_t x0 = 20, x1 = 228, yt = 56, yb = 76, hx = 70, ym = (yt + yb) / 2;
    uint32_t nh = 1u + (sys.fx_struct & 127u) * 3u / 128u, i;   /* the half waves: the JET's overblowing */
    int32_t px = 0, py = 0;
    art_line(x0, yt, hx - 6, yt);                            /* the bore, the embouchure open in its top */
    art_line(hx + 6, yt, x1, yt);
    art_line(x0, yb, x1, yb);
    art_line(x0, yt, x0, yb);                                /* the cork */
    for (i = 0; i < 4u; i++)                                 /* the tone holes */
        art_arc(130 + (int32_t)i * 22, yt - 1, 3, 180, 360, art_c, 0);
    art_arc(hx - 30, yt - 25, 5, -90, 90, art_c, 1);         /* the player's lips over it */
    art_arc(hx - 30, yt - 15, 5, -90, 90, art_c, 1);
    art_line(hx - 30, yt - 30, hx - 36, yt - 40);            /* the nose's line above, the chin's below */
    art_line(hx - 30, yt - 10, hx - 36, yt);
    {   /* the jet: from the lips to the edge, its wave growing along it */
        int32_t jx0 = hx - 25, jy0 = yt - 20, k;
        for (k = 0; k <= 16; k++) {
            int32_t x = jx0 + (hx + 6 - jx0) * k / 16, y = jy0 + (yt - jy0) * k / 16;
            y += art_lv * k * art_sin(art_t * 0x50000000u - (uint32_t)k * 0x18000000u) / (32768 * 16 * 18);
            if (k)
                art_line(px, py, x, y);
            px = x;
            py = y;
        }
    }
    {   /* the standing wave inside: nh half waves, pressure nodes at both open ends */
        int32_t a = art_lv * 8 / 256;
        for (i = 0; i <= 48u; i++) {
            int32_t x = hx + (x1 - hx) * (int32_t)i / 48;
            uint32_t u = (uint32_t)((uint64_t)i * nh * 0x80000000u / 48u);
            int32_t y = ym + a * art_sin(u) / 32768 * art_cos(art_t * 0x30000000u) / 32768;
            if (i)
                cv_line(px, py, x, y, ux_mix(T_BG, T_THEME, 66));
            px = x;
            py = y;
        }
    }
    {   /* the breath: dust around the jet, as much as BREATH and the level */
        uint32_t n = (uint32_t)art_lv * (sys.fx_pos & 127u) / 2048u, r = art_t * 2654435761u;
        for (i = 0; i < n; i++) {
            r = r * 1664525u + 1013904223u;
            cv_pset(hx - 22 + (int32_t)(r >> 27), yt - 24 + (int32_t)((r >> 23) & 15u), art_c);
        }
    }
    art_waves(x1 - 4, ym, 30, 40);
}

/* -------------------------------------------------------- SYRINX and HAWK --- */
static void art_bird(int hawk)
{
    const int32_t hx = 150, hy = 34;                         /* the head's centre */
    int32_t open = 2 + art_lv * 10 / 256, sx = 104, sy = 104, i;
    int32_t beat = (int32_t)(sys.fx_pos & 127u);
    /* the head */
    art_arc(hx, hy, 18, 150, 330, art_c, 1);
    art_dot(hx + 6, hy - 4);                                 /* the eye */
    if (hawk) {                                              /* the brow over it, the hooked beak */
        art_line(hx - 4, hy - 9, hx + 14, hy - 9);
        art_line(hx + 16, hy - 6, hx + 30, hy - 2);
        art_arc(hx + 30, hy + 6, 8, 270, 450, art_c, 1);     /* the hook */
        art_line(hx + 30, hy + 14, hx + 24, hy + 8);
        art_line(hx + 24, hy + 8, hx + 16, hy + 6);
        art_line(hx + 16, hy + 8 + open / 2, hx + 26, hy + 12 + open);   /* the lower mandible */
        art_line(hx + 26, hy + 12 + open, hx + 14, hy + 14 + open / 2);
    } else {                                                 /* a songbird's straight beak, opening */
        art_line(hx + 16, hy - 6, hx + 40, hy + 2 - open / 3);
        art_line(hx + 40, hy + 2 - open / 3, hx + 16, hy + 4);
        art_line(hx + 16, hy + 6, hx + 36, hy + 8 + open);
        art_line(hx + 36, hy + 8 + open, hx + 14, hy + 14);
    }
    /* the neck and the breast */
    art_line(hx - 16, hy + 10, sx - 30, PLOT_H);
    art_line(hx + 12, hy + 16, hx + 4, sy - 20);
    art_arc(hx - 26, sy - 20, 30, 0, 90, art_c, 1);
    /* the trachea down the neck to the syrinx, the bronchi forking from it */
    for (i = -1; i <= 1; i += 2) {
        art_thin(hx + 2 + i * 3, hy + 14, sx + i * 3, sy, art_c);
        art_thin(sx + i * 3, sy, sx + i * 14, PLOT_H - 2, art_c);
    }
    {   /* the two membranes at the fork: each side its own voice (TWO VOICE: the second apart) */
        int32_t lv = art_lv;
        uint32_t ph1 = art_t * 0x3C000000u, ph2 = art_t * (0x3C000000u + (uint32_t)beat * 0x00400000u);
        int32_t r1 = 2 + lv * (art_sin(ph1) + 32768) / (65536 * 48);
        int32_t r2 = 2 + lv * (art_sin(ph2) + 32768) / (65536 * 48);
        if (hawk && lv > 16) {                               /* (the rasp: shaking) */
            uint32_t r = art_t * 2246822519u;
            r1 += (int32_t)((r >> 29) & 3u) * (int32_t)(sys.fx_pos & 127u) / 64;
            r2 += (int32_t)((r >> 26) & 3u) * (int32_t)(sys.fx_pos & 127u) / 64;
        }
        art_arc(sx - 4, sy + 4, r1 + 2, 90, 270, art_c, 1);
        art_arc(sx + 4, sy + 4, r2 + 2, -90, 90, art_c, 1);
    }
    cv_text(8, sy - 6, &AF_S, "SYRINX", T_DIM);
    art_thin(56, sy + 2, sx - 10, sy + 4, T_LINE);
    art_waves(hx + 48, hy + 8, 35, 60);
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
    if (pk < 24 && sys.fx_mix)                               /* (silence: no level at all, not the noise floor) */
        pk = rz_view.wave[(w - 1u) & (RZ_WAVE - 1u)] ? pk : 0;
    art_lv = pk > art_lv ? pk : art_lv - (art_lv - pk) / 6;
    art_t++;
    art_c = sys.fx_mix ? T_THEME : ux_mix(T_BG, T_THEME, 30);
    cv_begin(240, PLOT_H, T_BG);
    switch (sys.fx_model % RZ_COUNT) {
    case RZ_MODAL: art_bell(); break;
    case RZ_SYMP: art_sitar(); break;
    case RZ_STRING: art_string_model(); break;
    case RZ_THROAT: art_throat(); break;
    case RZ_FLUTE: art_flute(); break;
    case RZ_SYRINX: art_bird(0); break;
    default: art_bird(1); break;
    }
    if (!sys.fx_mix)
        cv_text(8, 4, &AF_S, "MIX OFF", T_MID);
    cv_blit(0, PLOT_Y);
}
