/* SPDX-License-Identifier: GPL-3.0-only */
/* NOISE: thirty noise oscillators after the programs of Befaco's Noise Plethora (its manual describes each one; these
 * are this firmware's own fixed-point versions of those descriptions, written for a keyboard: the note sets the main
 * frequency, X and Y the program's two controls). Each voice runs its own copy, through the same ADSR and low pass
 * gate as the CHUA circuit.
 *
 *   A  TEXTURES        0 RADIO OH NO   1 RWALK FLANGE   2 XMOD RING SQR  3 XMOD RING SINE  4 CROSS MOD RING
 *                      5 RESONOISE     6 GRAIN GLITCH   7 GRAIN GLITCH 2 8 GRAIN GLITCH 3  9 BASURILLA
 *   B  HH CLUSTERS     0 CLUSTER SAW   1 PW CLUSTER     2 CR CLUSTER     3 SINE FM CLUST   4 TRI FM CLUST
 *                      5 PRIME CLUSTER 6 PRIME C NOISE  7 FIBONACCI      8 PARTIAL CLUST   9 PHASING CLUST
 *   C  HARSH & WILD    0 BASURA TOTAL  1 ATARI          2 WAKING FILOM.  3 P S H           4 ARRAY ROCKS
 *                      5 EXIST. PAIN   6 WHO KNOWS      7 SATAN WORKOUT  8 RWALK BITCRUSH  9 RWALK LFREE
 *
 * Building blocks: phase accumulators (Q32 cycles), square / pulse / saw / triangle / sine (px_sin), xorshift noise,
 * random walkers in a box, a Chamberlin state-variable band-pass, a granular cell (a 1024-sample buffer read back
 * faster in grains), a flanger, a 16-bit LFSR, sample & hold, a bit crusher and a small "dirty" reverb (three allpasses
 * and a damped comb). Signals Q15. Each voice has 4096 samples of buffer in the pool (np_buf).
 *
 * The FM oscillator (the MOD page): per voice a modulator at a ratio of the note, its wave morphing sine ->
 * triangle -> saw -> square, moves the pitch of every audio-rate oscillator of the program, linear FM (1 + m x depth;
 * no through zero: down to 1/16 of the frequency). At DEPTH 0 nothing of it runs.
 *
 *   np_start(k, seed)                 a voice's state at a new note
 *   np_render(k, alg, inc, out, n)    voice k, program alg (0..29), the note's increment: n samples Q15 into out */
#define NP_COUNT 30
static const char *const NP_NAME[NP_COUNT] = {
    "RADIO OH NO", "RWALK FLANGE", "XMOD RING SQR", "XMOD RING SINE", "CROSS MOD RING",
    "RESONOISE", "GRAIN GLITCH", "GRAIN GLITCH 2", "GRAIN GLITCH 3", "BASURILLA",
    "CLUSTER SAW", "PW CLUSTER", "CR CLUSTER", "SINE FM CLUST", "TRI FM CLUST",
    "PRIME CLUSTER", "PRIME C NOISE", "FIBONACCI", "PARTIAL CLUST", "PHASING CLUST",
    "BASURA TOTAL", "ATARI", "WAKING FILOM.", "P S H", "ARRAY ROCKS",
    "EXIST. PAIN", "WHO KNOWS", "SATAN WORKOUT", "RWALK BITCRUSH", "RWALK LFREE",
};
static const char *const NP_X[NP_COUNT] = {        /* what X and Y do, per program (the screen) */
    "OSC 2", "WALK RATE", "OSC 2", "OSC 2", "RATIOS", "MOD RATIO", "GRAIN", "GRAIN", "GRAIN", "LFO RATE",
    "SPREAD", "DETUNE", "DETUNE", "DETUNE", "DETUNE", "SPREAD", "SPREAD", "SPREAD", "SPREAD", "SPREAD",
    "LFSR BEND", "OSC 2", "BOX", "RATE", "MOD RATIO", "SWEEP RATE", "SWEEP RATE", "FM", "BOX", "BOX",
};
static const char *const NP_Y[NP_COUNT] = {
    "PWM", "FLANGE", "FM", "FM", "FM", "FOLD", "FM", "FM", "FM", "WIDTH",
    "VOICES", "PW", "FM", "FM", "FM", "FM", "NOISE FM", "TONE", "TONE", "DRIFT",
    "REVERB", "FM", "FM", "REVERB", "SCRAMBLE", "DEPTH", "DEPTH", "CRUSH", "REVERB", "REVERB",
};
/* the output level per program: measured (tests/basis_test.c, NP_LEVELS=1 prints them) so that each sounds about as
 * loud as the circuit at X = Y = 64, a four-note chord (Q12) */
static const uint16_t NP_GAIN[NP_COUNT] = {
    1747, 1703, 1041, 2220, 1945, 510, 880, 879, 1531, 4523,
    3027, 1975, 1996, 2339, 2423, 2275, 2289, 2350, 2309, 1750,
    4035, 1169, 1285, 3241, 1548, 959, 382, 925, 2334, 1740,
};

#define NP_BUF 4096u
/* a voice's buffer: the reverb's allpasses and comb, the granular cell, the flanger (a program uses a few of them) */
#define NB_AP1 0u
#define NB_AP2 241u
#define NB_AP3 624u
#define NB_COMB 1181u
#define NB_GRAIN 2800u
#define NB_FLANGE 3824u
#define NL_AP1 241u
#define NL_AP2 383u
#define NL_AP3 557u
#define NL_COMB 1617u
#define NL_GRAIN 1024u
#define NL_FLANGE 256u
static int16_t np_buf[NVOICE][NP_BUF] __attribute__((section(".pool")));

typedef struct {
    uint32_t ph[16], mp[6];                      /* oscillators, modulators */
    int32_t walk[16];                            /* random walkers (Q16, -1 .. 1) */
    uint32_t rng;
    int32_t last[4];                             /* the last outputs (for feedback, cross modulation) */
    int32_t lo[4], bd[4];                        /* band-passes: low, band */
    int32_t sh, lp, cnt, hold;                   /* sample & hold, one-pole, counters */
    uint32_t gw, gr, gstart, gn;                 /* the granular cell: write, read (Q16), grain start, samples left */
    uint32_t a1, a2, a3, cp;                     /* the reverb's positions */
    int32_t clp;                                 /* its comb's damping */
    uint32_t fw;                                 /* the flanger's write */
    uint32_t lfsr;
    int16_t wt[64];                              /* ARRAY ROCKS: the wavetable */
    int32_t hist[16];                            /* the last outputs every 4 samples (the screen's trace) */
    uint32_t hn;
    uint32_t mph;                                /* the FM oscillator's phase */
} np_t;
static np_t np_state[NVOICE];

/* ---------------------------------------------------------------- helpers --- */
static inline uint32_t np_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline int32_t np_noise(uint32_t *s) { return (int32_t)(np_rnd(s) >> 16) - 32768; }
static inline int32_t np_sq(uint32_t ph) { return ph < 0x80000000u ? 32767 : -32767; }
static inline int32_t np_pulse(uint32_t ph, uint32_t pw) { return ph < pw ? 32767 : -32767; }
static inline int32_t np_saw(uint32_t ph) { return (int32_t)(ph >> 16) - 32768; }
static inline int32_t np_tri(uint32_t ph)
{
    int32_t t = (int32_t)(ph >> 15);             /* 0 .. 131071 */
    return t < 65536 ? t - 32768 : 98303 - t;
}
/* a cheap sine (some programs take a dozen per sample): 256 points, linear between them, Q15 */
static const int16_t NP_SIN[257] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
    0,
};
static inline int32_t np_sin(uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t a = NP_SIN[i], f = (int32_t)((ph >> 9) & 32767u);
    return a + (((NP_SIN[i + 1u] - a) * f) >> 15);
}
static inline uint32_t np_mul(uint32_t inc, uint32_t r16) { return (uint32_t)(((uint64_t)inc * r16) >> 16); }
/* FM: the increment moved by mod (Q15) x index (Q15, 1.0 = +-100 % of the increment) */
static inline uint32_t np_fm(uint32_t inc, int32_t mod, int32_t idx)
{
    int32_t d = (int32_t)(((int64_t)(int32_t)(inc >> 1) * mod * idx) >> 29);
    return d < -(int32_t)(inc - (inc >> 4)) ? inc >> 4 : inc + (uint32_t)d;   /* (never at a standstill: an */
}                                                                           /* oscillator stuck would go silent) */
/* a ratio from a knob: 2^(lo + (hi - lo) k / 32767) octaves, Q16 */
static inline uint32_t np_ratio(int32_t k, int32_t lo_oct, int32_t hi_oct)
{
    return px_exp2(lo_oct * 65536 + (int32_t)(((int64_t)(hi_oct - lo_oct) * 65536 * k) / 32767));
}
static inline int32_t np_clip(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

/* the FM oscillator's wave at phase ph, Q15: w 0..127 morphs sine (0) -> triangle (42) -> saw (85) -> square (127),
 * each crossfaded into the next; all rise through 0 at phase 0 */
static uint32_t np_morph(uint32_t w)             /* the knob -> segment << 8 | fraction (0..255): the corners at */
{                                                /* 0, 42, 85, 127 exactly */
    w = w > 127u ? 127u : w;
    return w < 42u ? w * 256u / 42u : w < 85u ? 256u + (w - 42u) * 256u / 43u : w < 127u ? 512u + (w - 85u) * 256u / 42u
                                                                                       : 768u;
}
static int32_t np_wave(uint32_t ph, uint32_t w)
{
    int32_t a, b;
    uint32_t t = np_morph(w), f = t & 255u;
    switch (t >> 8) {
    default: a = np_sin(ph); b = np_tri(ph + 0x40000000u); break;
    case 1: a = np_tri(ph + 0x40000000u); b = np_saw(ph + 0x80000000u); break;
    case 2: a = np_saw(ph + 0x80000000u); b = np_sq(ph); break;
    case 3: return np_sq(ph);
    }
    return a + (((b - a) * (int32_t)f) >> 8);
}
#define NP_NRATIO 16
static const char *const NP_RATIO_NAME[NP_NRATIO] = {"1/4", "1/2", "3/4", "1", "5/4", "3/2", "2", "5/2", "3", "7/2",
                                                     "4", "5", "6", "7", "8", "11"};
static const uint32_t NP_RATIO[NP_NRATIO] = {16384, 32768, 49152, 65536, 81920, 98304, 131072, 163840, 196608,
                                             229376, 262144, 327680, 393216, 458752, 524288, 720896};   /* Q16 */
/* a random walker in a box of +-box (Q16): steps of +-step, reflected at the walls */
static inline void np_walk(int32_t *w, uint32_t *rng, int32_t step, int32_t box)
{
    int32_t v = *w + (int32_t)(((int64_t)np_noise(rng) * step) >> 15);
    if (v > box) v = 2 * box - v;
    if (v < -box) v = -2 * box - v;
    *w = clamp(v, -box, box);
}
/* a Chamberlin band-pass: f = 2 sin(pi fc / FS) ~ 2 pi inc / 2^32 (Q16, clamped stable), damping q Q16 */
static inline int32_t np_bp(int32_t *lo, int32_t *bd, int32_t in, uint32_t inc, int32_t q)
{
    int32_t f = (int32_t)(((uint64_t)inc * 411775u) >> 32), hi;
    f = f > 48000 ? 48000 : f;
    *lo += (int32_t)(((int64_t)f * *bd) >> 16);
    hi = in - *lo - (int32_t)(((int64_t)q * *bd) >> 16);
    *bd += (int32_t)(((int64_t)f * hi) >> 16);
    *bd = clamp(*bd, -(1 << 20), 1 << 20);
    *lo = clamp(*lo, -(1 << 20), 1 << 20);
    return *bd;
}
/* the small reverb: three allpasses (0.6) then a comb whose feedback (room, Q15) is damped; returns the wet */
static int32_t np_verb(np_t *S, int16_t *b, int32_t in, int32_t room)
{
    int32_t d, v, c;
    d = b[NB_AP1 + S->a1]; v = in + (d * 19661 >> 15); b[NB_AP1 + S->a1] = (int16_t)np_clip(v); in = d - (v * 19661 >> 15);
    S->a1 = S->a1 + 1u == NL_AP1 ? 0u : S->a1 + 1u;
    d = b[NB_AP2 + S->a2]; v = in + (d * 19661 >> 15); b[NB_AP2 + S->a2] = (int16_t)np_clip(v); in = d - (v * 19661 >> 15);
    S->a2 = S->a2 + 1u == NL_AP2 ? 0u : S->a2 + 1u;
    d = b[NB_AP3 + S->a3]; v = in + (d * 19661 >> 15); b[NB_AP3 + S->a3] = (int16_t)np_clip(v); in = d - (v * 19661 >> 15);
    S->a3 = S->a3 + 1u == NL_AP3 ? 0u : S->a3 + 1u;
    c = b[NB_COMB + S->cp];
    S->clp += (c - S->clp) * 3 / 8;              /* (damping: the tail darkens) */
    b[NB_COMB + S->cp] = (int16_t)np_clip(in / 2 + (int32_t)(((int64_t)S->clp * room) >> 15));
    S->cp = S->cp + 1u == NL_COMB ? 0u : S->cp + 1u;
    return c + in / 2;
}
/* the granular cell: in goes in; read back at rate (Q16) in grains of len samples, each from len samples back */
static int32_t np_grain(np_t *S, int16_t *b, int32_t in, uint32_t rate, uint32_t len)
{
    int32_t o;
    b[NB_GRAIN + (S->gw & (NL_GRAIN - 1u))] = (int16_t)np_clip(in);
    S->gw++;
    if (!S->gn) {                                /* a new grain */
        S->gn = len;
        S->gr = (S->gw - len - 1u) << 16;
    }
    S->gn--;
    o = b[NB_GRAIN + ((S->gr >> 16) & (NL_GRAIN - 1u))];
    S->gr += rate;
    return o;
}

static void np_start(uint32_t k, uint32_t seed)   /* (seed: from note_on's rng(); under a held note, from the */
{                                                  /* voice: the second core must not touch rng()) */
    np_t *S = &np_state[k];
    uint32_t i;
    seed ^= 0x9E3779B9u * (k + 1u);
    memset(S, 0, sizeof *S);
    memset(np_buf[k], 0, sizeof np_buf[k]);
    S->rng = seed | 1u;
    S->lfsr = 0xACE1u;
    for (i = 0; i < 16u; i++)
        S->ph[i] = np_rnd(&S->rng);              /* (clusters: no click of all phases at 0) */
    for (i = 0; i < 64u; i++)
        S->wt[i] = (int16_t)np_noise(&S->rng);
}

static const uint16_t NP_PRIMES[16] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53};
static const uint16_t NP_FIB[16] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597};
static const uint32_t NP_MODR[6] = {65536, 92682, 131072, 180879, 196608, 32768};   /* 1, 1.41, 2, 2.76, 3, 0.5 */

/* ---------------------------------------------------------------- render --- */
static void np_render(uint32_t k, uint32_t alg, uint32_t inc, int32_t *out, uint32_t n)
{
    np_t *S = &np_state[k];
    int16_t *b = np_buf[k];
    int32_t x = (int32_t)(sys.np_x & 127u) * 258, y = (int32_t)(sys.np_y & 127u) * 258;   /* Q15 0 .. 32766 */
    uint32_t i, j;
    uint32_t fk[CTL], fm = sys.mod_depth & 127u;
    if (inc > 0x20000000u)                       /* (an eighth of the rate: room for the ratios above it) */
        inc = 0x20000000u;
    if (n > CTL)
        n = CTL;
    if (fm) {   /* the FM oscillator: per sample the factor on every audio-rate increment, Q16 */
        uint32_t mi = np_mul(np_mul(inc, NP_RATIO[sys.mod_ratio % NP_NRATIO]),
                             px_exp2(((int32_t)(sys.mod_fine & 127u) - 64) * 1365 / 16)),   /* +-1 semitone */
                 wv = sys.mod_wave & 127u;
        int32_t d = (int32_t)(fm * fm * 3u / 2u);   /* depth^2 x 1.5: 0 .. 24193 (Q14: 0 .. 1.48) */
        for (i = 0; i < n; i++) {
            int32_t f = 65536 + ((np_wave(S->mph, wv) * d) >> 13);
            S->mph += mi;
            fk[i] = f < 4096 ? 4096u : (uint32_t)f;
        }
    }
#define NPF(v) (fm ? np_mul((v), fk[i]) : (v))   /* an audio-rate increment, the FM on it */
    switch (alg) {
    /* ---------------------------------------------------------- A: textures --- */
    case 0: {   /* RADIO OH NO: four squares cross-modulating in pairs, summed */
        uint32_t r = np_ratio(x, -1, 2), i1 = np_mul(inc, r), i2 = np_mul(inc, 49152u), i3 = np_mul(i1, 87381u);
        uint32_t pw = 0x80000000u + (uint32_t)((y - 16384) * 98304);
        for (i = 0; i < n; i++) {
            int32_t o0 = np_sq(S->ph[0]), o1 = np_pulse(S->ph[1], pw), o2 = np_sq(S->ph[2]), o3 = np_pulse(S->ph[3], pw);
            S->ph[0] += NPF(np_fm(inc, o1, 16384));
            S->ph[1] += NPF(np_fm(i1, o0, 16384));
            S->ph[2] += NPF(np_fm(i2, o3, 16384));
            S->ph[3] += NPF(np_fm(i3, o2, 16384));
            out[i] = (o0 + o1 + o2 + o3) / 4;
        }
        break;
    }
    case 1: {   /* RWALK FLANGE: four random walkers tune pulse oscillators that FM a sine; a flanger after it */
        int32_t step = 4 + (x >> 7) * 4, depth = 8 + (y >> 10);
        for (i = 0; i < n; i++) {
            int32_t m = 0, s, d, fl;
            for (j = 0; j < 4u; j++) {
                np_walk(&S->walk[j], &S->rng, step, 65536);
                S->mp[j] += NPF(np_mul(inc, (uint32_t)(65536 + (S->walk[j] >> 1)) * (j + 1u) / 2u));
                m += np_pulse(S->mp[j], 0x80000000u);
            }
            S->ph[0] += NPF(np_fm(inc, m / 4, 20000));
            s = np_sin(S->ph[0]);
            b[NB_FLANGE + (S->fw & (NL_FLANGE - 1u))] = (int16_t)s;
            S->ph[1] += 3000u * 97u;                 /* the flanger's LFO, ~0.3 Hz */
            d = 2 + ((np_tri(S->ph[1]) + 32768) * depth >> 16);
            fl = b[NB_FLANGE + ((S->fw - (uint32_t)d) & (NL_FLANGE - 1u))];
            S->fw++;
            out[i] = (s + fl * y / 32767) / 2;
        }
        break;
    }
    case 2:     /* XMOD RING SQR: two squares FM each other; their ring modulation */
    case 3: {   /* XMOD RING SINE: the same with sines */
        uint32_t i1 = np_mul(inc, np_ratio(x, -2, 2));
        for (i = 0; i < n; i++) {
            int32_t o0 = alg == 2u ? np_sq(S->ph[0]) : np_sin(S->ph[0]);
            int32_t o1 = alg == 2u ? np_sq(S->ph[1]) : np_sin(S->ph[1]);
            S->ph[0] += NPF(np_fm(inc, o1, y));
            S->ph[1] += NPF(np_fm(i1, o0, y));
            out[i] = (o0 * o1) >> 15;
        }
        break;
    }
    case 4: {   /* CROSS MOD RING: sine, saw, square, triangle FM each other in a chain (last to first); their product */
        uint32_t r = 65536u + (uint32_t)x * 4u, i1 = np_mul(inc, r), i2 = np_mul(i1, r), i3 = np_mul(i2, r);
        for (i = 0; i < n; i++) {
            int32_t w0 = np_sin(S->ph[0]), w1 = np_saw(S->ph[1]), w2 = np_sq(S->ph[2]), w3 = np_tri(S->ph[3]), p;
            S->ph[0] += NPF(np_fm(inc, w3, y));
            S->ph[1] += NPF(np_fm(i1, w0, y));
            S->ph[2] += NPF(np_fm(i2, w1, y));
            S->ph[3] += NPF(np_fm(i3, w2, y));
            p = (w0 * w1) >> 15;
            p = (p * w2) >> 15;
            out[i] = (p * w3) >> 14;
        }
        break;
    }
    case 5: {   /* RESONOISE: a square FMs a sine, folded, through a band-pass whose cutoff white noise moves */
        uint32_t i1 = np_mul(inc, np_ratio(x, -1, 2));
        int32_t fold = 32768 + y * 8;                /* gain 1 .. 9 before the fold, Q15 */
        for (i = 0; i < n; i++) {
            int32_t m = np_sq(S->ph[1]), s, v;
            S->ph[1] += NPF(i1);
            S->ph[0] += NPF(np_fm(inc, m, 16384));
            s = (int32_t)(((int64_t)np_sin(S->ph[0]) * fold) >> 15);
            v = np_tri((uint32_t)s * 65536u + 0x40000000u);   /* the fold: a triangle of the scaled wave */
            out[i] = np_bp(&S->lo[0], &S->bd[0], v, np_mul(inc * 3u, (uint32_t)(65536 + np_noise(&S->rng))), 6000) * 2;
        }
        break;
    }
    case 6:     /* GRAIN GLITCH: a square through a granular cell, fed back as FM; the XOR of the two */
    case 7:     /* GRAIN GLITCH 2: the same, the cell's output */
    case 8: {   /* GRAIN GLITCH 3: a saw through it, the cell's output */
        uint32_t len = 32u + (uint32_t)(x >> 5);     /* 32 .. 1055 -> within the cell */
        len = len > NL_GRAIN - 2u ? NL_GRAIN - 2u : len;
        for (i = 0; i < n; i++) {
            int32_t o = alg == 8u ? np_saw(S->ph[0]) : np_sq(S->ph[0]);
            int32_t g = np_grain(S, b, o, 98304u, len);   /* read back 1.5x: a fifth up, glitching at each grain */
            S->ph[0] += NPF(np_fm(inc, g, y));
            out[i] = alg == 6u ? (o * g) >> 15 : g;     /* (XOR of two squares: their product) */
        }
        break;
    }
    case 9: {   /* BASURILLA: three white noises gated by three pulse LFOs */
        uint32_t base = np_mul(inc >> 6, np_ratio(x, -2, 3)), pw = 0x08000000u + (uint32_t)y * 0x7000u;
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            S->mp[0] += NPF(base);
            S->mp[1] += NPF(np_mul(base, 89784u));    /* x 1.37 */
            S->mp[2] += NPF(np_mul(base, 117309u));   /* x 1.79 */
            for (j = 0; j < 3u; j++)
                if (S->mp[j] < pw)
                    s += np_noise(&S->rng);
            out[i] = s / 2;
        }
        break;
    }
    /* -------------------------------------------------------- B: clusters --- */
    case 10: {  /* CLUSTER SAW: 16 saws a linear spread apart; Y how many */
        uint32_t nv = 2u + (uint32_t)(y * 14 / 32767), sp = (uint32_t)x / 8u;   /* (0 .. 0.0625 of f apart) */
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            for (j = 0; j < nv; j++) {
                S->ph[j] += NPF(np_mul(inc, 65536u + sp * j));
                s += np_saw(S->ph[j]);
            }
            out[i] = s / (int32_t)(2u + nv / 2u);
        }
        break;
    }
    case 11: {  /* PW CLUSTER: six detuned pulses, their pulse width Y */
        uint32_t pw = 0x10000000u + (uint32_t)y * 0x6000u;
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            for (j = 0; j < 6u; j++) {
                S->ph[j] += NPF(np_mul(inc, (uint32_t)(65536 + ((int32_t)j * 2 - 5) * (x >> 6))));
                s += np_pulse(S->ph[j], pw);
            }
            out[i] = s / 4;
        }
        break;
    }
    case 12:    /* CR CLUSTER: six detuned sines, a slow FM on all */
    case 13:    /* SINE FM CLUST: six detuned sines, each FMed by its own sine */
    case 14: {  /* TRI FM CLUST: six detuned triangles, each FMed by its own sine */
        for (i = 0; i < n; i++) {
            int32_t s = 0, slow;
            S->mp[5] += NPF(inc >> 4);
            slow = np_sin(S->mp[5]);
            for (j = 0; j < 6u; j++) {
                uint32_t ci = np_mul(inc, (uint32_t)(65536 + ((int32_t)j * 2 - 5) * (x >> 6)));
                int32_t m;
                if (alg == 12u)
                    m = slow;
                else {
                    S->mp[j] += NPF(np_mul(ci, NP_MODR[j]));
                    m = np_sin(S->mp[j]);
                }
                S->ph[j] += NPF(np_fm(ci, m, y));
                s += alg == 14u ? np_tri(S->ph[j]) : np_sin(S->ph[j]);
            }
            out[i] = s / 4;
        }
        break;
    }
    case 15:    /* PRIME CLUSTER: 16 saws detuned by the primes, one sine FMs them all */
    case 16: {  /* PRIME C NOISE: 16 triangles detuned by the primes, white noise FMs them */
        uint32_t sp = (uint32_t)x >> 6;              /* a prime's share, 0 .. 512 / 65536 */
        for (i = 0; i < n; i++) {
            int32_t s = 0, m;
            if (alg == 15u) {
                S->mp[0] += NPF(inc >> 1);
                m = np_sin(S->mp[0]);
            } else
                m = np_noise(&S->rng);
            for (j = 0; j < 16u; j++) {
                S->ph[j] += NPF(np_fm(np_mul(inc, 65536u + sp * NP_PRIMES[j]), m, y >> 1));
                s += alg == 15u ? np_saw(S->ph[j]) : np_tri(S->ph[j]);
            }
            out[i] = s / 6;
        }
        break;
    }
    case 17:    /* FIBONACCI: 16 saws detuned along the Fibonacci series */
    case 18: {  /* PARTIAL CLUST: 16 saws, each the one below times (1 + spread) */
        uint32_t sp = (uint32_t)x >> 7, mul = 65536u + (uint32_t)x / 2u, ci[16];
        int32_t tone = 2000 + y * 3 / 2;             /* the one-pole low-pass, Q15 */
        uint32_t c = inc;
        for (j = 0; j < 16u; j++) {
            if (alg == 17u)
                ci[j] = np_mul(inc, 65536u + sp * NP_FIB[j] / 8u);
            else {
                ci[j] = c;
                c = np_mul(c, mul);
                if (c > 0x30000000u)
                    c = 0x30000000u;
            }
        }
        for (i = 0; i < n; i++) {
            int32_t s = 0;
            for (j = 0; j < 16u; j++) {
                S->ph[j] += NPF(ci[j]);
                s += np_saw(S->ph[j]);
            }
            S->lp += (int32_t)(((int64_t)(s / 6 - S->lp) * tone) >> 15);
            out[i] = S->lp;
        }
        break;
    }
    case 19: {  /* PHASING CLUST: 16 squares a spread apart, a slow LFO drifting each */
        uint32_t sp = (uint32_t)x >> 7;
        for (i = 0; i < n; i++) {
            int32_t s = 0, lfo;
            S->mp[0] += 9000u;                       /* ~0.1 Hz */
            lfo = np_sin(S->mp[0]);
            for (j = 0; j < 16u; j++) {
                int32_t dr = (int32_t)(((int64_t)lfo * y * (int32_t)(j + 1u)) >> 30);   /* up to +-16 / 65536 x j */
                S->ph[j] += NPF(np_mul(inc, (uint32_t)(65536 + (int32_t)(sp * j) + dr * 4)));
                s += np_sq(S->ph[j]);
            }
            out[i] = s / 8;
        }
        break;
    }
    /* ---------------------------------------------------- C: harsh & wild --- */
    case 20: {  /* BASURA TOTAL: a bent LFSR clocked by an oscillator its own bits FM; reverb */
        for (i = 0; i < n; i++) {
            uint32_t prev = S->ph[0];
            int32_t bit = S->lfsr & 1u ? 32767 : -32767, w;
            S->ph[0] += NPF(np_fm(inc * 4u, bit, x));
            if (S->ph[0] < prev)                     /* a clock: shift the LFSR (Galois, taps 16 14 13 11) */
                S->lfsr = (S->lfsr >> 1) ^ ((0u - (S->lfsr & 1u)) & 0xB400u);
            w = np_verb(S, b, bit / 2, 16000 + y / 3);
            out[i] = (int32_t)(((int64_t)bit * (32767 - y) + (int64_t)w * y) >> 16);
        }
        break;
    }
    case 21: {  /* ATARI: two squares, each moving the other's pulse width and frequency */
        uint32_t i1 = np_mul(inc, np_ratio(x, -1, 2));
        for (i = 0; i < n; i++) {
            int32_t o0 = np_pulse(S->ph[0], 0x80000000u + (uint32_t)(S->last[1] * (y >> 2))),
                    o1 = np_pulse(S->ph[1], 0x80000000u + (uint32_t)(S->last[0] * (y >> 2)));
            S->ph[0] += NPF(np_fm(inc, o1, y >> 1));
            S->ph[1] += NPF(np_fm(i1, o0, y >> 1));
            S->last[0] = o0;
            S->last[1] = o1;
            out[i] = (o0 + o1) / 2;
        }
        break;
    }
    case 22: {  /* WAKING FILOM.: 16 random walkers in a box of X tune pulse oscillators, each FMing the next */
        int32_t box = 2000 + x * 3 / 2;              /* (at most ~0.78: no frequency below 0) */
        for (i = 0; i < n; i++) {
            int32_t s = 0, prev = S->last[0];
            for (j = 0; j < 16u; j++) {
                int32_t o;
                if (!(i & 7u))
                    np_walk(&S->walk[j], &S->rng, 600, box);
                S->ph[j] += NPF(np_fm(np_mul(inc, (uint32_t)(65536 + S->walk[j])), prev, y >> 2));
                o = np_pulse(S->ph[j], 0x80000000u);
                s += o;
                prev = o;
            }
            S->last[0] = prev;
            out[i] = s / 6;
        }
        break;
    }
    case 23: {  /* P S H: noise sampled and held at the note's rate (X its multiple); dirty reverb */
        uint32_t ri = np_mul(inc, np_ratio(x, -3, 2));
        for (i = 0; i < n; i++) {
            uint32_t prev = S->ph[0];
            int32_t w;
            S->ph[0] += NPF(ri);
            if (S->ph[0] < prev)
                S->sh = np_noise(&S->rng);
            w = np_verb(S, b, S->sh / 2, 26000);
            out[i] = (int32_t)(((int64_t)S->sh * (32767 - y) + (int64_t)w * y) >> 15);
        }
        break;
    }
    case 24: {  /* ARRAY ROCKS: a sine FMs a 64-step wavetable whose steps are scrambled as it plays */
        uint32_t i1 = np_mul(inc, np_ratio(x, -2, 2)), every = 4096u - (uint32_t)y / 9u;
        for (i = 0; i < n; i++) {
            int32_t m = np_sin(S->mp[0]);
            S->mp[0] += NPF(i1);
            S->ph[0] += NPF(np_fm(inc, m, 16384));
            out[i] = S->wt[S->ph[0] >> 26];
            if ((uint32_t)++S->cnt >= every) {       /* scramble a step */
                S->cnt = 0;
                S->wt[np_rnd(&S->rng) & 63u] = (int16_t)np_noise(&S->rng);
            }
        }
        break;
    }
    case 25:    /* EXIST. PAIN: sample & hold noise through four band-passes swept by four triangle LFOs */
    case 26: {  /* WHO KNOWS: a pulse wave through the same four swept band-passes */
        static const uint32_t BASE[4] = {0x01200000u, 0x02400000u, 0x04800000u, 0x09000000u};   /* ~200 .. 1600 Hz */
        static const uint16_t LR[4] = {300, 470, 710, 1130};   /* hundredths of a hertz */
        uint32_t lr = 1u + (uint32_t)x / 64u;
        for (i = 0; i < n; i++) {
            int32_t in, s = 0;
            uint32_t prev = S->ph[0];
            S->ph[0] += NPF(inc);
            if (alg == 25u) {
                if (S->ph[0] < prev)
                    S->sh = np_noise(&S->rng);
                in = S->sh;
            } else
                in = np_pulse(S->ph[0], 0x40000000u);
            for (j = 0; j < 4u; j++) {
                int32_t t;
                S->mp[j] += LR[j] * 10u * lr;            /* 0.3 .. 1.1 Hz, x 1 .. 512 */
                t = np_tri(S->mp[j]);
                s += np_bp(&S->lo[j], &S->bd[j], in, np_mul(BASE[j], (uint32_t)px_exp2((t * (y >> 6)) >> 7)), 3000);
            }
            out[i] = s / 2;
        }
        break;
    }
    case 27: {  /* SATAN WORKOUT: white noise FMs a sine, then down-sampled and driven into a clip */
        uint32_t every = 1u + (uint32_t)y / 1100u;
        for (i = 0; i < n; i++) {
            S->ph[0] += NPF(np_fm(inc, np_noise(&S->rng), x));
            if ((uint32_t)++S->cnt >= every) {
                S->cnt = 0;
                S->hold = np_clip(np_sin(S->ph[0]) * 4);
            }
            out[i] = S->hold;
        }
        break;
    }
    case 28: {  /* RWALK BITCRUSH: nine walkers in a box tune nine pulses (and their widths); crushed by the first */
        int32_t box = 2000 + x * 3 / 2;
        for (i = 0; i < n; i++) {
            int32_t s = 0, w;
            uint32_t bits;
            for (j = 0; j < 9u; j++) {
                if (!(i & 7u))
                    np_walk(&S->walk[j], &S->rng, 500, box);
                S->ph[j] += NPF(np_mul(inc, (uint32_t)(65536 + S->walk[j]) * (1u + j % 3u) / 2u));
                s += np_pulse(S->ph[j], 0x80000000u + (uint32_t)S->walk[j] * 16384u);
            }
            s /= 4;
            bits = 3u + (uint32_t)((S->walk[0] + box) * 9 / (2 * box + 1));   /* 3 .. 12 bits */
            s &= ~((1 << (16u - bits)) - 1);         /* (a mask: shifting a negative left is undefined) */
            w = np_verb(S, b, s / 2, 12000 + y / 2);
            out[i] = (s + w) / 2;
        }
        break;
    }
    default: {  /* RWALK LFREE: four pulses, frequency and width walking in a box; reverb */
        int32_t box = 2000 + x * 3 / 2;
        for (i = 0; i < n; i++) {
            int32_t s = 0, w;
            for (j = 0; j < 4u; j++) {
                if (!(i & 3u))
                    np_walk(&S->walk[j], &S->rng, 900, box);
                S->ph[j] += NPF(np_mul(inc, (uint32_t)(65536 + S->walk[j]) * (j + 2u) / 3u));
                s += np_pulse(S->ph[j], 0x80000000u + (uint32_t)S->walk[(j + 1u) & 3u] * 16384u);
            }
            w = np_verb(S, b, s / 4, 12000 + y / 2);
            out[i] = (s / 2 + w) / 2;
        }
        break;
    }
    }
#undef NPF
    {   /* the level */
        int32_t g = (int32_t)NP_GAIN[alg % NP_COUNT] * (int32_t)(sys.np_level & 127u) / 64;
        for (i = 0; i < n; i++)
            out[i] = np_clip((int32_t)(((int64_t)out[i] * g) >> 12));
    }
}
