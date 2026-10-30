/* SPDX-License-Identifier: GPL-3.0-only */
/* SHIMMER: a stereo reverb with a pitch shifter in its feedback loop. Every trip round the loop part of the tail is
 * shifted up by the INTERVAL and fed back in, so the tail climbs: an octave, then another, and with SHIMMER high and a
 * long DECAY it keeps rising and building, a riser, until TONE's damping takes the top off.
 *
 *   input   two allpasses diffuse the dry signal
 *   tank    a 4-line feedback delay network (Householder matrix: o - sum / 2), each line with a one-pole low-pass
 *           (TONE) and the gain of its length for the DECAY time; SIZE scales the lengths (they glide when it turns)
 *   shimmer the tank's outputs, high-passed (~120 Hz: no rumble climbing), through a two-tap granular pitch shifter
 *           (taps sweeping a 2048-sample window, crossfaded by triangles): SHIMMER crossfades part of the tank's
 *           feedback from itself to this shifted copy, keeping its energy (weights a and b, a^2 + b^2 = 1), so the
 *           loop gain stays the DECAY's (bounded). Each trip round (~60 .. 110 ms) that part of the tail moves up an
 *           interval: at SHIMMER 127 (30 % a trip) about 2..3 octaves a second, the lower octaves fading as the higher
 *           ones build
 *   output  left and right from different lines; dry / wet by MIX (the dry stays full to the middle)
 *
 *   SIZE 0..127      the room: 0.35 .. 1 of the longest lines (~60 .. 110 ms)
 *   DECAY 0..127     RT60 0.4 s .. 30 s
 *   SHIMMER 0..127   how much of the feedback is the shifted tail: how fast it rises (0: a plain reverb)
 *   MIX 0..127       0: off (nothing runs; the tank is cleared at the next use)
 *   INTERVAL         +OCT, +5TH, +OCT+5TH, +2 OCT
 *   TONE 0..127      the tank's damping: dark (1.5 kHz) .. open (16 kHz)
 *   SPACE            the building it rings in (ALGORITHM, with the INTERVAL): HALL (the plain tank), TEMPLE (a stone
 *                    chamber: dense early reflections, the hypogeum's resonance ~110 Hz and its neighbours, dark),
 *                    CATHEDRAL (the nave: echoes off the pillars 23 .. 89 ms, the long low modes 49 / 98 / 147 Hz),
 *                    MOSQUE (the dome: a flutter echo focused back and forth, its ringing modes 165 / 330 / 495 Hz,
 *                    bright)
 * Fixed point: signals Q15 in int32, the lines int16 (saturating), coefficients Q15 / Q16; the modes Q28.
 *
 *   fx_shimmer(in, l, r, n)   audio ISR, core 0: in[] mono Q15 -> l[], r[] Q15 */
#define SH_NLINE 4
#define SH_MAXLEN 4800u                          /* the longest line (at SIZE 127) */
static const uint16_t SH_LEN[SH_NLINE] = {4799, 3931, 3203, 2687};   /* mutually prime: no shared echoes */
#define SH_AP1 142u
#define SH_AP2 379u
#define SH_PS 4096u                              /* the pitch shifter's buffer (a power of two) */
#define SH_WIN 2048u                             /* its window */
enum { SI_OCT, SI_FIFTH, SI_OCT5, SI_2OCT, SI_COUNT };
static const char *const SH_INTERVAL_NAME[SI_COUNT] = {"+OCT", "+5TH", "+OCT+5TH", "+2 OCT"};
static const uint32_t SH_INC[SI_COUNT] = {1u << 21, 1u << 20, 1u << 22, 3u << 21};   /* (ratio - 1) 2^32 / SH_WIN */
enum { SS_HALL, SS_TEMPLE, SS_CATHEDRAL, SS_MOSQUE, SS_COUNT };
static const char *const SH_SPACE_NAME[SS_COUNT] = {"HALL", "TEMPLE", "CATHEDRAL", "MOSQUE"};
/* each space: its three modes (two-pole resonators: a1 = 2 r cos w, a2 = r^2, b = (1 - r) 2 sin w x 2 gain: the peak
 * twice the gain; Q28), its early
 * reflections (taps in ms, gains Q15; MOSQUE: one tap fed back, the flutter), its damping (x / 16 of TONE's) */
static const int32_t SH_MODE[SS_COUNT][3][3] = {
    {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    {{536777290, 268368535, 667}, {536711510, 268341983, 2930}, {536510598, 268259584, 4611}},
    {{536782875, 268360505, 628}, {536668693, 268285575, 3349}, {536528380, 268210666, 4709}},
    {{536512317, 268225188, 7910}, {535857546, 268015084, 23718}, {534907193, 267805145, 35554}},
};
static const uint8_t SH_ER_MS[SS_COUNT][4] = {{0, 0, 0, 0}, {7, 13, 19, 29}, {23, 41, 67, 89}, {31, 0, 0, 0}};
static const uint16_t SH_ER_G[SS_COUNT][4] = {{0, 0, 0, 0}, {16384, 13107, 9830, 6554}, {14746, 13107, 11469, 9830},
                                              {18022, 0, 0, 0}};
static const uint8_t SH_DAMP16[SS_COUNT] = {16, 9, 12, 20};
#define SH_ER 4096u                              /* the early reflections' line (93 ms, a power of two) */

static struct {
    int16_t line[SH_NLINE][SH_MAXLEN];
    int16_t ap1[SH_AP1], ap2[SH_AP2];
    int16_t ps[SH_PS];
    int16_t er[SH_ER];
    int32_t my[3][2];                            /* the modes' last two outputs */
    uint32_t ew;
    uint32_t wp[SH_NLINE], a1, a2, pw;
    uint32_t len[SH_NLINE];                      /* the read lengths now (gliding to SIZE's) */
    int32_t lp[SH_NLINE];                        /* the damping filters */
    int32_t hp_x, hp_y;                          /* the shifter's high-pass */
    int32_t dc[2], dcx[2];                       /* the wet output's DC blockers (~23 Hz), Q15 x 256 */
    uint32_t ph;                                 /* the shifter's phase, Q32 */
    uint8_t on;                                  /* the tank holds something: cleared when MIX goes to 0 */
} sh __attribute__((section(".pool")));

/* for the screen (ui.c): the wet level per block, a ring the main loop reads */
#define SH_ENV 64u
static struct { uint16_t env[SH_ENV]; volatile uint32_t w; } sh_view;

static inline int16_t sh_sat(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* a soft limit for the shifted feedback: linear to 16000, then towards 32000 */
static inline int32_t sh_soft(int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    if (a > 16000) {
        uint32_t e = (uint32_t)(a - 16000) > 131071u ? 131071u : (uint32_t)(a - 16000);   /* (32-bit division: the */
        a = 16000 + (int32_t)(e * 16000u / (16000u + e));                                   /* toolchain has no 64) */
        v = v < 0 ? -a : a;
    }
    return v;
}

static int32_t sh_dry = 32768;                   /* this block's dry level, Q15 (synth.c pans the dry with it) */
static void fx_shimmer(const int32_t *in, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, k, size = sys.sh_size & 127u, iv = sys.sh_interval % SI_COUNT, sp = sys.sh_space % SS_COUNT;
    uint32_t etap[4];
    int32_t mix = sys.sh_mix > 127u ? 127 : (int32_t)sys.sh_mix, g[SH_NLINE], damp, fa, fb_, dry, wet;
    uint32_t inc = SH_INC[iv], peak = 0;
    sh_dry = 32768;
    if (!mix) {
        for (i = 0; i < n; i++)
            l[i] = r[i] = in[i];
        if (sh.on) {                             /* (off: the next use starts from silence) */
            memset(&sh, 0, sizeof sh);
        }
        return;
    }
    if (!sh.on) {
        for (k = 0; k < SH_NLINE; k++)
            sh.len[k] = SH_LEN[k];
        sh.on = 1;
    }
    {   /* per block: the lengths glide to SIZE's, the gains of the DECAY time, TONE, SHIMMER */
        uint32_t scale = 22938u + size * 330u;   /* 0.35 .. 1, Q16 */
        uint32_t rt = (uint32_t)(((uint64_t)17640u * px_exp2((int32_t)((sys.sh_decay & 127u) * 408220u / 127u))) >> 16);
        int32_t fc_oct = (int32_t)((sys.sh_tone & 127u) * 222822u / 127u);   /* 1.5 kHz x 2^(0 .. 3.4) */
        uint32_t fc = (uint32_t)(((uint64_t)1500u * px_exp2(fc_oct)) >> 16) * SH_DAMP16[sp] / 16u;
        for (k = 0; k < 4u; k++)                 /* the early reflections' taps, samples */
            etap[k] = (uint32_t)SH_ER_MS[sp][k] * 441u / 10u;
        for (k = 0; k < SH_NLINE; k++) {
            uint32_t t = (uint32_t)(((uint64_t)SH_LEN[k] * scale) >> 16);
            int32_t d = (int32_t)t - (int32_t)sh.len[k];
            sh.len[k] = (uint32_t)((int32_t)sh.len[k] + (d > 16 ? 16 : d < -16 ? -16 : d));
            /* gain 2^(-9.966 len / rt) (60 dB in rt samples) */
            g[k] = (int32_t)(px_exp2(-(int32_t)((653126u * sh.len[k]) / rt)) >> 1);   /* Q15 (< 2^32: 32-bit division) */
        }
        /* the damping one-pole: a = 1 - exp(-2 pi fc / FS) = 1 - 2^(-2 pi fc / (FS ln 2)) */
        damp = 32768 - (int32_t)(px_exp2(-(int32_t)(((uint64_t)fc * 13794u) >> 10)) >> 1);
        {   /* the crossfade: c = 0 .. 0.3 of the amplitude; a = 1 - c, b = sqrt(1 - a^2) x 1.22 (the shifter's two
             * triangle-weighted taps keep ~2/3 of the energy of uncorrelated input) */
            int32_t c = (int32_t)(sys.sh_shimmer & 127u) * 77;           /* Q15, 0 .. 0.3 */
            uint32_t a2;
            fa = 32768 - c;
            a2 = (uint32_t)(((int64_t)fa * fa) >> 15);                   /* a^2, Q15 */
            fb_ = (int32_t)px_isqrt64((uint64_t)(32768u - a2) << 15);    /* Q15 */
            fb_ = (int32_t)(((int64_t)fb_ * 40132) >> 15);                /* x 1.22 */
        }
        dry = mix < 64 ? 32768 : (127 - mix) * 32768 / 63;
        wet = mix > 64 ? 32768 : mix * 32768 / 64;
        sh_dry = dry;
    }
    for (i = 0; i < n; i++) {
        int32_t x = in[i] >> 1, o[SH_NLINE], s, ps, v, y;
        /* input diffusion: two allpasses (gain 0.6) */
        {
            int32_t d1 = sh.ap1[sh.a1], d2;
            v = x + (d1 * 19661 >> 15);
            sh.ap1[sh.a1] = sh_sat(v);
            x = d1 - (v * 19661 >> 15);
            sh.a1 = sh.a1 + 1u == SH_AP1 ? 0u : sh.a1 + 1u;
            d2 = sh.ap2[sh.a2];
            v = x + (d2 * 19661 >> 15);
            sh.ap2[sh.a2] = sh_sat(v);
            x = d2 - (v * 19661 >> 15);
            sh.a2 = sh.a2 + 1u == SH_AP2 ? 0u : sh.a2 + 1u;
        }
        /* the building: early reflections (MOSQUE: the flutter, fed back) into the tank's input */
        if (sp != SS_HALL) {
            int32_t e = 0, w = in[i] >> 1;
            for (k = 0; k < 4u; k++)
                if (etap[k])
                    e += sh.er[(sh.ew - etap[k]) & (SH_ER - 1u)] * (int32_t)SH_ER_G[sp][k] >> 15;
            if (sp == SS_MOSQUE)
                w += e;
            sh.er[sh.ew & (SH_ER - 1u)] = sh_sat(w);
            sh.ew++;
            x += e / 2;
        }
        /* the tank's outputs, damped */
        for (k = 0; k < SH_NLINE; k++) {
            uint32_t rd = sh.wp[k] + SH_MAXLEN - sh.len[k];
            int32_t t = sh.line[k][rd >= SH_MAXLEN ? rd - SH_MAXLEN : rd];
            sh.lp[k] += (int32_t)(((int64_t)(t - sh.lp[k]) * damp + 16384) >> 15);   /* (rounded: no DC builds) */
            o[k] = sh.lp[k];
        }
        /* the shifter: its input the tail, high-passed (one pole, ~120 Hz) */
        {
            int32_t hx = (o[0] + o[1] - o[2] - o[3]) / 2, a, b;   /* (the signs the shifted tail goes in with) */
            uint32_t ph2, d1, d2, w1, w2;
            sh.hp_y = hx - sh.hp_x + (int32_t)(((int64_t)sh.hp_y * 32210 + 16384) >> 15);
            sh.hp_x = hx;
            sh.ps[sh.pw & (SH_PS - 1u)] = sh_sat(sh.hp_y);
            /* two taps, half a window apart: delay = SH_WIN (1 - phase), falling: the pitch rises */
            ph2 = sh.ph + 0x80000000u;
            d1 = (65536u - (sh.ph >> 16)) * SH_WIN;  /* Q16 samples */
            d2 = (65536u - (ph2 >> 16)) * SH_WIN;
            {
                uint32_t p1 = sh.pw - 1u - (d1 >> 16), p2 = sh.pw - 1u - (d2 >> 16);
                int32_t f1 = (int32_t)(d1 & 0xFFFFu), f2 = (int32_t)(d2 & 0xFFFFu);
                int32_t s0 = sh.ps[p1 & (SH_PS - 1u)], s1 = sh.ps[(p1 - 1u) & (SH_PS - 1u)];
                int32_t t0 = sh.ps[p2 & (SH_PS - 1u)], t1 = sh.ps[(p2 - 1u) & (SH_PS - 1u)];
                a = s0 + (int32_t)(((int64_t)(s1 - s0) * f1 + 32768) >> 16);   /* (rounded throughout: a floor */
                b = t0 + (int32_t)(((int64_t)(t1 - t0) * f2 + 32768) >> 16);   /* bias would build DC in the loop) */
            }
            w1 = sh.ph >> 16;                    /* triangle windows: 1 - |2 p - 1|, Q16; the two sum to 1 */
            w1 = w1 < 32768u ? w1 * 2u : (65535u - w1) * 2u;
            w2 = 65536u - w1;
            ps = sh_soft((int32_t)(((int64_t)a * w1 + (int64_t)b * w2 + 32768) >> 16));
            sh.ph += inc;
            sh.pw++;
        }
        /* the feedback: the Householder mix of the lines and the shifted tail, crossfaded by SHIMMER, at the gain of
         * the DECAY; then the input (signs alternate: decorrelated) */
        s = (o[0] + o[1] + o[2] + o[3]) / 2;
        for (k = 0; k < SH_NLINE; k++) {
            int32_t h = o[k] - s, q = k & 2u ? -ps : ps;
            int32_t fb = (int32_t)(((int64_t)h * fa + (int64_t)q * fb_ + 16384) >> 15);
            int32_t f = (int32_t)(((int64_t)fb * g[k] + 16384) >> 15) + (k & 1u ? -x : x);
            sh.line[k][sh.wp[k]] = sh_sat(f);
            sh.wp[k] = sh.wp[k] + 1u == SH_MAXLEN ? 0u : sh.wp[k] + 1u;
        }
        /* out: left and right from different lines */
        v = o[0] - o[2] + o[1] / 2;
        y = o[1] - o[3] + o[0] / 2;
        if (sp != SS_HALL) {                     /* the building's modes, rung by the input and the tail */
            int32_t e = x + s / 4, m = 0;
            for (k = 0; k < 3u; k++) {
                const int32_t *c = SH_MODE[sp][k];
                int32_t z = (int32_t)(((int64_t)c[0] * sh.my[k][0] - (int64_t)c[1] * sh.my[k][1] +
                                       (int64_t)c[2] * e) >> 28);
                z = z > 60000 ? 60000 : z < -60000 ? -60000 : z;
                sh.my[k][1] = sh.my[k][0];
                sh.my[k][0] = z;
                m += z;
            }
            v += m / 2;
            y += m / 2;
        }
        {   /* DC blockers on the wet, y = x - x' + R y' (R ~ 1 - 2 pi 23 Hz / FS), states x 256 for precision */
            int32_t xv = v * 256, xy = y * 256;
            sh.dc[0] = xv - sh.dcx[0] + (int32_t)(((int64_t)sh.dc[0] * 32622 + 16384) >> 15);
            sh.dcx[0] = xv;
            sh.dc[1] = xy - sh.dcx[1] + (int32_t)(((int64_t)sh.dc[1] * 32622 + 16384) >> 15);
            sh.dcx[1] = xy;
            v = sh.dc[0] / 420;                  /* (/ 256, x 0.6: three lines summed come out ~1.6x the dry) */
            y = sh.dc[1] / 420;
        }
        l[i] = (int32_t)(((int64_t)in[i] * dry + (int64_t)v * wet) >> 15);
        r[i] = (int32_t)(((int64_t)in[i] * dry + (int64_t)y * wet) >> 15);
        {
            uint32_t a2 = (uint32_t)(v < 0 ? -v : v) + (uint32_t)(y < 0 ? -y : y);
            peak = a2 > peak ? a2 : peak;
        }
    }
    sh_view.env[sh_view.w++ & (SH_ENV - 1u)] = (uint16_t)(peak > 65535u ? 65535u : peak);
}
