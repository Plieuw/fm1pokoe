/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX waveguides: three more models for resonator.c, beside Rings' (the same page, the same polyphony: one per
 * note, fed by the voices that play it). Digital waveguides: travelling waves in delay lines, scattered where the
 * tube changes, reflected at its ends. This firmware's own fixed-point code (after the textbook models: Kelly and
 * Lochbaum's vocal tract, Cook's slide flute / STK Flute's jet and bore), signals Q20 like Rings' models.
 *
 *   THROAT   a vocal tract: a tube of up to 24 sections (Kelly-Lochbaum), the voices its glottal source. ANIMAL sets
 *            the tube's length (its formants: a cat's short throat, a cow's long one, an elephant's longer still)
 *            and how much the mouth opens with the voice's loudness ("meow", "moo"); VOWEL (POSITION) the tongue:
 *            U, O, A, E, I, morphing; BRIGHTNESS the mouth's radiation; DAMPING how long the tube rings (the
 *            glottis' and the lips' reflections)
 *   FLUTE    a jet over the mouth of a bore tuned to the note: the voices' loudness is the breath that blows it,
 *            their sound rides on the breath (as STK's Flute: jet delay, the jet's x (x^2 - 1), the bore's low-pass
 *            reflection). JET (STRUCTURE) the jet's length: long blows the fundamental, short overblows to octaves
 *            and twelfths; BREATH (POSITION) the breath noise; BRIGHTNESS the bore's loss of highs; DAMPING its loss
 *   SYRINX   a bird's two-sided voice box: two small whistles (jets on short bores) two octaves above the note, their
 *            pitch rising with the breath (CHIRP: STRUCTURE), the second a little apart (TWO VOICE: POSITION: beats
 *            .. a fifth), through the beak's resonance (BRIGHTNESS: 1.5 .. 6 kHz); DAMPING the whistles' loss
 *
 * The breath: each model follows its input's loudness (an envelope, ~2 ms up, ~80 ms down); the flute and the
 * syrinx blow only while it plays, the throat's mouth moves with it. */

/* ------------------------------------------------------------------ THROAT --- */
#define WG_NSEC 24                               /* sections at most */
#define WG_KMAX 6                                /* samples per section at most (a section: k samples each way) */
enum { WA_CAT, WA_MONKEY, WA_HUMAN, WA_DOG, WA_WOLF, WA_COW, WA_LION, WA_ELEPHANT, WA_COUNT };
static const char *const WG_ANIMAL[WA_COUNT] = {"CAT", "MONKEY", "HUMAN", "DOG", "WOLF", "COW", "LION", "ELEPHANT"};
static const struct {
    uint8_t n, k;                                /* sections, samples each: the tube n k / 44100 s one way (x 350 m/s) */
    uint8_t wah;                                 /* how much the mouth follows the loudness, /128 */
    uint8_t nasal;                               /* the glottis' reflection's floor, /128 (a "closed", nasal tone) */
    uint8_t gain;                                /* the level out (measured: about the dry's, tests/basis_test.c) */
} WG_ANI[WA_COUNT] = {
    {10, 1, 120, 90, 17},                        /* cat: 8 cm, the mouth opening with the meow */
    {14, 1, 90, 70, 25},                         /* monkey: 11 cm, hoots */
    {21, 1, 40, 60, 33},                         /* human: 17 cm */
    {14, 2, 70, 70, 20},                         /* dog: 22 cm, barks open */
    {20, 2, 60, 80, 10},                         /* wolf: 32 cm, the howl's slow vowel */
    {24, 2, 80, 100, 7},                         /* cow: 38 cm, "moo": nasal, closed then open */
    {20, 3, 50, 70, 11},                         /* lion: 48 cm, a roar */
    {22, 5, 60, 110, 6},                         /* elephant: 87 cm (the trunk), a low rumble */
};
/* the area along the tube, glottis to lips, per vowel (relative, Q4; after Fant's measured shapes, simplified) */
#define WG_NV 5
static const uint8_t WG_SHAPE[WG_NV][16] = {
    {19, 26, 38, 51, 58, 51, 38, 19, 10, 13, 26, 38, 26, 10, 5, 3},       /* U: lips rounded, nearly closed */
    {16, 22, 32, 38, 38, 32, 22, 16, 16, 26, 38, 48, 42, 29, 16, 10},     /* O */
    {16, 13, 10, 8, 8, 10, 14, 22, 35, 48, 58, 64, 67, 64, 58, 51},       /* A: the pharynx narrow, the mouth wide */
    {19, 29, 42, 51, 54, 48, 38, 29, 22, 16, 13, 13, 16, 22, 29, 32},     /* E */
    {22, 42, 58, 67, 70, 64, 48, 32, 16, 8, 5, 4, 5, 10, 19, 26},         /* I: the tongue high, front narrow */
};
static const char *const WG_VOWEL[WG_NV] = {"U", "O", "A", "E", "I"};

typedef struct {
    int32_t f[WG_NSEC][WG_KMAX], b[WG_NSEC][WG_KMAX];   /* each section: its forward and backward waves */
    uint32_t p;                                  /* the place in the sections' lines (0 .. k - 1) */
    int32_t r[WG_NSEC];                          /* the junctions' reflections (Q15), this block */
    int32_t lip, rad, glo;                       /* the lips' low-pass, the mouth's softening, the glottis' */
    int32_t env;                                 /* the input's loudness, Q20 */
} wg_throat_t;

/* the loudness of n samples of in (Q20) into *env: ~2 ms up, ~80 ms down (per sample) */
static int32_t wg_follow(int32_t *env, const int32_t *in, uint32_t n)
{
    uint32_t i;
    int32_t e = *env;
    for (i = 0; i < n; i++) {
        int32_t a = in[i] < 0 ? -in[i] : in[i];
        e += a > e ? (a - e) / 64 : -((e - a) / 4096) - (e > 0);
    }
    return *env = e < 0 ? 0 : e;
}

static void wg_throat_run(wg_throat_t *T, const int32_t *in, int32_t *y, uint32_t n, uint32_t animal, int32_t vowel,
                          int32_t bright, int32_t damp)
{
    uint32_t an = animal % WA_COUNT, ns = WG_ANI[an].n, k = WG_ANI[an].k, i, j;
    int32_t env = wg_follow(&T->env, in, n), A[WG_NSEC + 1], open, rg, rl, lp, ro;
    {   /* the areas: the vowel (0..127 across U O A E I), resampled to the sections; the mouth (the last quarter)
         * opened by the loudness as much as the animal does it */
        int32_t pos = vowel * 4 * 256 / 127, v = pos >> 8, fr = pos & 255;
        int32_t e = env > PX_ONE / 2 ? 256 : env / (PX_ONE / 512);   /* 0..256: loud at half scale */
        v = v > WG_NV - 2 ? WG_NV - 2 : v;
        if (pos >= (WG_NV - 1) * 256)
            fr = 256;
        open = (128 - WG_ANI[an].wah) * 2 + WG_ANI[an].wah * (40 + e * 3 / 2) / 128;   /* /256 */
        for (j = 0; j < ns; j++) {
            uint32_t s = j * 15u * 16u / (ns - 1u), s0 = s >> 4, s1 = s0 < 15u ? s0 + 1u : 15u, sf = s & 15u;
            int32_t a0 = WG_SHAPE[v][s0] * (16 - (int32_t)sf) + WG_SHAPE[v][s1] * (int32_t)sf;
            int32_t a1 = WG_SHAPE[v + 1][s0] * (16 - (int32_t)sf) + WG_SHAPE[v + 1][s1] * (int32_t)sf;
            int32_t a = (a0 * (256 - fr) + a1 * fr) >> 8;
            if (j * 4u >= ns * 3u)
                a = a * open >> 8;
            A[j] = a < 40 ? 40 : a;              /* (never shut: a closed mouth would trap a whistle) */
        }
        for (j = 0; j + 1u < ns; j++)            /* Kelly-Lochbaum: r = (A_j - A_j+1) / (A_j + A_j+1) */
            T->r[j] = (A[j] - A[j + 1]) * 32767 / (A[j] + A[j + 1]);
    }
    rg = 22000 + WG_ANI[an].nasal * 40 + damp / 11;   /* the glottis: ~0.67 .. 0.99 (Q15) */
    rg = rg > 32440 ? 32440 : rg;
    rl = -(26000 + damp / 10);                   /* the lips (open end: inverted), 0.79 .. 0.99 */
    rl = rl < -32440 ? -32440 : rl;
    lp = 2500 + bright / 5;                      /* the lips' low-pass, Q15: what reflects (the lows), ~0.4 .. 4 kHz */
    ro = 6800 + bright / 3;                      /* the mouth's softening, Q15 */
    for (i = 0; i < n; i++) {
        int32_t fo[WG_NSEC], bo[WG_NSEC], lo;
        uint32_t p = T->p;
        for (j = 0; j < ns; j++) {
            fo[j] = T->f[j][p];
            bo[j] = T->b[j][p];
        }
        for (j = 0; j + 1u < ns; j++) {          /* the junctions */
            int32_t w = px_m(T->r[j], fo[j] - bo[j + 1u], 15);
            T->f[j + 1u][p] = px_clamp(fo[j] + w, -16 * PX_ONE, 16 * PX_ONE);
            T->b[j][p] = px_clamp(bo[j + 1u] + w, -16 * PX_ONE, 16 * PX_ONE);
        }
        T->glo += px_m(14000, bo[0] - T->glo, 15);   /* the glottis: soft (the highs lost there, ~3 kHz) */
        T->f[0][p] = px_clamp((in[i] >> 1) + px_m(rg, T->glo, 15), -16 * PX_ONE, 16 * PX_ONE);
        lo = fo[ns - 1u];                        /* the lips: the lows reflect (inverted), the highs radiate */
        T->lip += px_m(lp, lo - T->lip, 15);
        T->b[ns - 1u][p] = px_m(rl, T->lip, 15);
        /* out at the mouth: the wave arriving there (the radiation's loss of the lows left out: it would leave a
         * voice below its first formant nearly silent), its top softened (BRIGHTNESS: ~1.5 .. 12 kHz) */
        T->rad += px_m(ro, lo - T->rad, 15);
        y[i] = px_clamp(T->rad * WG_ANI[an].gain, -64 * PX_ONE, 64 * PX_ONE);
        T->p = p + 1u >= k ? 0u : p + 1u;
    }
}

/* ---------------------------------------------------- the jet (FLUTE, SYRINX) --- */
/* (the lines are clipped at +-5, not 4: -4.0 in Q20 is 0xFFC00000, which tools/build.py takes for a ROM address) */
static inline int32_t wg_jet(int32_t x)          /* STK's JetTable: x (x^2 - 1), the output clipped to +-1 (Q20) */
{
    int32_t c = px_clamp(x, -2 * PX_ONE, 2 * PX_ONE);
    return px_clamp(px_m(c, px_m(c, c, 20) - PX_ONE, 20), -PX_ONE, PX_ONE);
}
static inline int32_t wg_read(const int32_t *l, uint32_t mask, uint32_t w, uint32_t d)   /* d samples back, Q16 */
{
    uint32_t t = w - (d >> 16);
    int32_t a = l[(t - 1u) & mask], b = l[(t - 2u) & mask];
    return a + px_m(b - a, (int32_t)(d & 0xFFFFu), 16);
}

/* ------------------------------------------------------------------- FLUTE --- */
#define WG_BORE 1024u                            /* the bore's line (1.5 periods): 66 Hz and up (lower: up) */
#define WG_JETL 512u
typedef struct {
    int32_t bore[WG_BORE], jet[WG_JETL];
    uint32_t w, rng;
    int32_t lp, dcx, dcy, env;
    int32_t ox, oy;                              /* the output's DC blocker (the breath's pressure is DC in the bore) */
} wg_flute_t;

/* f0 the note (cycles Q32); jet 0..127 its length (long .. overblowing); breath the noise; bright, damp Q16 */
static void wg_flute_run(wg_flute_t *F, const int32_t *in, int32_t *y, uint32_t n, uint32_t f0, int32_t jet,
                         int32_t breath, int32_t bright, int32_t damp)
{
    uint32_t i, per = f0 < 65536u ? 0xFFFFFFFFu : px_period(f0 > 0x40000000u ? 0x40000000u : f0), bd, jd;   /* Q16 */
    int32_t env = wg_follow(&F->env, in, n), pr, g, loss, nz;
    while (per > (WG_BORE * 2u / 3u - 8u) << 16)  /* fold up an octave */
        per >>= 1;
    bd = per > (4u << 16) ? per - (2u << 16) : 2u << 16;   /* the bore: a period (less the filters' delay) */
    jd = (uint32_t)(((uint64_t)bd * (uint32_t)(16384 - jet * 90)) >> 15);   /* 0.5 .. 0.15 of the bore: the
                                                  * fundamental (bending sharp), the octave, the twelfth */
    jd = jd > (WG_JETL - 4u) << 16 ? (WG_JETL - 4u) << 16 : jd < (2u << 16) ? 2u << 16 : jd;
    pr = env * 16 > PX_ONE * 13 / 10 ? PX_ONE * 13 / 10 : env * 16;   /* the breath: the loudness (STK blows at ~1.1 ..
                                                  * 1.3: it speaks from ~1/16 of full scale up) */
    g = 4900 + bright / 5;                       /* the bore's one-pole (STK: pole 0.65): 0.85 .. 0.45, Q15 */
    loss = 30500 + (int32_t)(((int64_t)damp * 2260) >> 16);   /* 0.93 .. 1.0 */
    nz = breath / 2;                             /* the noise: 0 .. 0.5 of the pressure (STK: 0.15), Q16 */
    for (i = 0; i < n; i++) {
        int32_t b = wg_read(F->bore, WG_BORE - 1u, F->w, bd), t, pd, j, o;
        int32_t noise = px_m(px_m((int32_t)(px_rand(&F->rng) >> 11) - (1 << 20), pr, 20), nz, 16);
        F->lp += px_m(g, b - F->lp, 15);         /* the open end's reflection: low-passed, its DC blocked */
        t = F->lp;
        o = t - F->dcx + px_m(F->dcy, 32604, 15);
        F->dcx = t;
        F->dcy = o;
        t = px_m(o, loss, 15);
        pd = pr + noise + (in[i] >> 3) - (t >> 1);   /* the breath (the voices on it), less the jet's reflection */
        F->jet[F->w & (WG_JETL - 1u)] = px_clamp(pd, -5 * PX_ONE, 5 * PX_ONE);
        j = wg_jet(wg_read(F->jet, WG_JETL - 1u, F->w + 1u, jd));
        F->bore[F->w & (WG_BORE - 1u)] = px_clamp(j + (t >> 1), -5 * PX_ONE, 5 * PX_ONE);
        F->w++;
        o = b - F->ox + px_m(F->oy, 32604, 15); /* out: the bore, its DC blocked (the breath is DC in it) */
        F->ox = b;
        F->oy = o;
        y[i] = o;
    }
}

/* ------------------------------------------------------------------ SYRINX --- */
#define WG_SB 128u                               /* each whistle's bore (345 Hz and up) */
#define WG_SJ 64u
typedef struct {
    int32_t bore[2][WG_SB], jet[2][WG_SJ];
    uint32_t w, rng;
    int32_t lp[2], dcx[2], dcy[2], env;
    int32_t blo, bbd;                            /* the beak: a resonant band-pass (Chamberlin) */
    int32_t ox, oy;                              /* the output's DC blocker */
} wg_syrinx_t;

static void wg_syrinx_run(wg_syrinx_t *S, const int32_t *in, int32_t *y, uint32_t n, uint32_t f0, int32_t chirp,
                          int32_t two, int32_t bright, int32_t damp)
{
    uint32_t i, s, bd[2], jd[2];
    int32_t env = wg_follow(&S->env, in, n), pr, loss, bf;
    int32_t e = env > PX_ONE / 12 ? 65536 : env * 12 / 16;   /* 0..1 Q16: the breath, full at 1/12 scale */
    pr = env * 16 > PX_ONE * 13 / 10 ? PX_ONE * 13 / 10 : env * 16;
    for (s = 0; s < 2u; s++) {   /* the pitch: two octaves up, rising with the breath; the second apart */
        int32_t oct = 2 * 65536 + (int32_t)(((int64_t)chirp * 1032 * e) >> 16)     /* up to +2 octaves */
                      + (s ? two * 301 : 0);                                      /* +0 .. 7 semitones */
        uint64_t f = ((uint64_t)f0 * px_exp2(oct)) >> 16;
        uint32_t per = f < 65536u ? 0xFFFFFFFFu : f >= 0x40000000u ? 4u << 16 : px_period((uint32_t)f);
        while (per > (WG_SB - 8u) << 16)
            per >>= 1;
        bd[s] = per > (4u << 16) ? per - (2u << 16) : 2u << 16;
        jd[s] = bd[s] / 3u < (2u << 16) ? 2u << 16 : bd[s] / 3u;
        jd[s] = jd[s] > (WG_SJ - 4u) << 16 ? (WG_SJ - 4u) << 16 : jd[s];
    }
    loss = 29000 + (int32_t)(((int64_t)damp * 3400) >> 16);
    bf = 14000 + bright / 6;                     /* the beak: 2 sin(pi f / FS), ~1.5 .. 6 kHz (Q15) */
    for (i = 0; i < n; i++) {
        int32_t o = 0, hi;
        for (s = 0; s < 2u; s++) {
            int32_t b = wg_read(S->bore[s], WG_SB - 1u, S->w, bd[s]), t, pd, j;
            int32_t noise = px_m((int32_t)(px_rand(&S->rng) >> 12) - (1 << 19), pr, 22);
            S->lp[s] += px_m(20000, b - S->lp[s], 15);
            t = S->lp[s] - S->dcx[s] + px_m(S->dcy[s], 32604, 15);
            S->dcx[s] = S->lp[s];
            S->dcy[s] = t;
            t = px_m(t, loss, 15);
            pd = pr + noise + (in[i] >> 4) - (t >> 1);
            S->jet[s][S->w & (WG_SJ - 1u)] = px_clamp(pd, -5 * PX_ONE, 5 * PX_ONE);
            j = wg_jet(wg_read(S->jet[s], WG_SJ - 1u, S->w + 1u, jd[s]));
            S->bore[s][S->w & (WG_SB - 1u)] = px_clamp(j + (t >> 1), -5 * PX_ONE, 5 * PX_ONE);
            o += b;
        }
        S->w++;
        S->blo += px_m(bf, S->bbd, 15);          /* the beak (Q 2.5) */
        hi = o - S->blo - px_m(13107, S->bbd, 15);
        S->bbd = px_clamp(S->bbd + px_m(bf, hi, 15), -16 * PX_ONE, 16 * PX_ONE);
        S->blo = px_clamp(S->blo, -16 * PX_ONE, 16 * PX_ONE);
        S->oy = (o >> 2) - S->ox + px_m(S->oy, 32604, 15);
        S->ox = o >> 2;
        y[i] = S->bbd + S->oy;
    }
}
